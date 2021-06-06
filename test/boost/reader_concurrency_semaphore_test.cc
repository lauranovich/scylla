/*
 * Copyright (C) 2021-present ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "reader_concurrency_semaphore.hh"
#include "test/lib/simple_schema.hh"
#include "test/lib/eventually.hh"
#include "test/lib/random_utils.hh"

#include <seastar/core/coroutine.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include <boost/test/unit_test.hpp>

SEASTAR_THREAD_TEST_CASE(test_reader_concurrency_semaphore_clear_inactive_reads) {
    simple_schema s;
    std::vector<reader_concurrency_semaphore::inactive_read_handle> handles;

    {
        reader_concurrency_semaphore semaphore(reader_concurrency_semaphore::no_limits{}, get_name());
        auto stop_sem = deferred_stop(semaphore);

        for (int i = 0; i < 10; ++i) {
            handles.emplace_back(semaphore.register_inactive_read(make_empty_flat_reader(s.schema(), semaphore.make_permit(s.schema().get(), get_name()))));
        }

        BOOST_REQUIRE(std::all_of(handles.begin(), handles.end(), [] (const reader_concurrency_semaphore::inactive_read_handle& handle) { return bool(handle); }));

        semaphore.clear_inactive_reads();

        BOOST_REQUIRE(std::all_of(handles.begin(), handles.end(), [] (const reader_concurrency_semaphore::inactive_read_handle& handle) { return !bool(handle); }));

        handles.clear();

        for (int i = 0; i < 10; ++i) {
            handles.emplace_back(semaphore.register_inactive_read(make_empty_flat_reader(s.schema(), semaphore.make_permit(s.schema().get(), get_name()))));
        }

        BOOST_REQUIRE(std::all_of(handles.begin(), handles.end(), [] (const reader_concurrency_semaphore::inactive_read_handle& handle) { return bool(handle); }));
    }

    // Check that the destructor also clears inactive reads.
    BOOST_REQUIRE(std::all_of(handles.begin(), handles.end(), [] (const reader_concurrency_semaphore::inactive_read_handle& handle) { return !bool(handle); }));
}

SEASTAR_THREAD_TEST_CASE(test_reader_concurrency_semaphore_destroyed_permit_releases_units) {
    simple_schema s;
    const auto initial_resources = reader_concurrency_semaphore::resources{10, 1024 * 1024};
    reader_concurrency_semaphore semaphore(initial_resources.count, initial_resources.memory, get_name());
    auto stop_sem = deferred_stop(semaphore);

    // Not admitted, active
    {
        auto permit = semaphore.make_permit(s.schema().get(), get_name());
        auto units2 = permit.consume_memory(1024);
    }
    BOOST_REQUIRE(semaphore.available_resources() == initial_resources);

    // Not admitted, inactive
    {
        auto permit = semaphore.make_permit(s.schema().get(), get_name());
        auto units2 = permit.consume_memory(1024);

        auto handle = semaphore.register_inactive_read(make_empty_flat_reader(s.schema(), permit));
        BOOST_REQUIRE(semaphore.try_evict_one_inactive_read());
    }
    BOOST_REQUIRE(semaphore.available_resources() == initial_resources);

    // Admitted, active
    {
        auto permit = semaphore.make_permit(s.schema().get(), get_name());
        auto units1 = permit.wait_admission(1024, db::no_timeout).get0();
        auto units2 = permit.consume_memory(1024);
    }
    BOOST_REQUIRE(semaphore.available_resources() == initial_resources);

    // Admitted, inactive
    {
        auto permit = semaphore.make_permit(s.schema().get(), get_name());
        auto units1 = permit.wait_admission(1024, db::no_timeout).get0();
        auto units2 = permit.consume_memory(1024);

        auto handle = semaphore.register_inactive_read(make_empty_flat_reader(s.schema(), permit));
        BOOST_REQUIRE(semaphore.try_evict_one_inactive_read());
    }
    BOOST_REQUIRE(semaphore.available_resources() == initial_resources);
}

SEASTAR_THREAD_TEST_CASE(test_reader_concurrency_semaphore_abandoned_handle_closes_reader) {
    simple_schema s;
    reader_concurrency_semaphore semaphore(reader_concurrency_semaphore::no_limits{}, get_name());
    auto stop_sem = deferred_stop(semaphore);

    auto permit = semaphore.make_permit(s.schema().get(), get_name());
    {
        auto handle = semaphore.register_inactive_read(make_empty_flat_reader(s.schema(), permit));
        // The handle is destroyed here, triggering the destrution of the inactive read.
        // If the test fails an assert() is triggered due to the reader being
        // destroyed without having been closed before.
    }
}

// This unit test passes a read through admission again-and-again, just
// like an evictable reader would be during its lifetime. When readmitted
// the read sometimes has to wait and sometimes not. This is to check that
// the readmitting a previously admitted reader doesn't leak any units.
SEASTAR_THREAD_TEST_CASE(test_reader_concurrency_semaphore_readmission_preserves_units) {
    simple_schema s;
    const auto initial_resources = reader_concurrency_semaphore::resources{10, 1024 * 1024};
    reader_concurrency_semaphore semaphore(initial_resources.count, initial_resources.memory, get_name());

    auto permit = semaphore.make_permit(s.schema().get(), get_name());
    auto stop_sem = deferred_stop(semaphore);

    std::optional<reader_permit::resource_units> residue_units;

    for (int i = 0; i < 10; ++i) {
        const auto have_residue_units = bool(residue_units);

        auto current_resources = initial_resources;
        if (have_residue_units) {
            current_resources -= residue_units->resources();
        }
        BOOST_REQUIRE(semaphore.available_resources() == current_resources);

        std::optional<reader_permit::resource_units> admitted_units;
        if (i % 2) {
            const auto consumed_resources = semaphore.available_resources();
            semaphore.consume(consumed_resources);

            auto units_fut = permit.wait_admission(1024, db::no_timeout);
            BOOST_REQUIRE(!units_fut.available());

            semaphore.signal(consumed_resources);
            admitted_units = units_fut.get();
        } else {
            admitted_units = permit.wait_admission(1024, db::no_timeout).get();
        }

        current_resources -= admitted_units->resources();
        BOOST_REQUIRE(semaphore.available_resources() == current_resources);

        residue_units.emplace(permit.consume_resources(reader_resources(0, 100)));
        if (!have_residue_units) {
            current_resources -= residue_units->resources();
        }
        BOOST_REQUIRE(semaphore.available_resources() == current_resources);

        auto handle = semaphore.register_inactive_read(make_empty_flat_reader(s.schema(), permit));
        BOOST_REQUIRE(semaphore.try_evict_one_inactive_read());
    }

    BOOST_REQUIRE(semaphore.available_resources() == initial_resources - residue_units->resources());

    residue_units.reset();

    BOOST_REQUIRE(semaphore.available_resources() == initial_resources);
}

// This unit test checks that the semaphore doesn't get into a deadlock
// when contended, in the presence of many memory-only reads (that don't
// wait for admission). This is tested by simulating the 3 kind of reads we
// currently have in the system:
// * memory-only: reads that don't pass admission and only own memory.
// * admitted: reads that pass admission.
// * evictable: admitted reads that are furthermore evictable.
//
// The test creates and runs a large number of these reads in parallel,
// read kinds being selected randomly, then creates a watchdog which
// kills the test if no progress is being made.
SEASTAR_THREAD_TEST_CASE(test_reader_concurrency_semaphore_forward_progress) {
    class reader {
        class skeleton_reader : public flat_mutation_reader::impl {
            reader_permit::resource_units _base_resources;
            std::optional<reader_permit::resource_units> _resources;
        public:
            skeleton_reader(schema_ptr s, reader_permit permit, reader_permit::resource_units res)
                : impl(std::move(s), std::move(permit)), _base_resources(std::move(res)) { }
            virtual future<> fill_buffer(db::timeout_clock::time_point timeout) override {
                _resources.emplace(_permit.consume_resources(reader_resources(0, tests::random::get_int(1024, 2048))));
                return make_ready_future<>();
            }
            virtual future<> next_partition() override { return make_ready_future<>(); }
            virtual future<> fast_forward_to(const dht::partition_range& pr, db::timeout_clock::time_point timeout) override { return make_ready_future<>(); }
            virtual future<> fast_forward_to(position_range, db::timeout_clock::time_point timeout) override { return make_ready_future<>(); }
            virtual future<> close() noexcept override {
                _resources.reset();
                return make_ready_future<>();
            }
        };
        struct reader_visitor {
            reader& r;
            future<> operator()(std::monostate& ms) { return r.tick(ms); }
            future<> operator()(flat_mutation_reader& reader) { return r.tick(reader); }
            future<> operator()(reader_concurrency_semaphore::inactive_read_handle& handle) { return r.tick(handle); }
        };

    private:
        schema_ptr _schema;
        reader_permit _permit;
        bool _memory_only = true;
        bool _evictable = false;
        std::optional<reader_permit::resource_units> _units;
        std::variant<std::monostate, flat_mutation_reader, reader_concurrency_semaphore::inactive_read_handle> _reader;

    private:
        future<> make_reader() {
            auto res = _permit.consume_memory();
            if (!_memory_only) {
                res = co_await _permit.wait_admission(1024, db::no_timeout);
            }
            _reader = make_flat_mutation_reader<skeleton_reader>(_schema, _permit, std::move(res));
        }
        future<> tick(std::monostate&) {
            co_await make_reader();
            co_await tick(std::get<flat_mutation_reader>(_reader));
        }
        future<> tick(flat_mutation_reader& reader) {
            co_await reader.fill_buffer(db::no_timeout);
            if (_evictable) {
                _reader = _permit.semaphore().register_inactive_read(std::move(reader));
            }
        }
        future<> tick(reader_concurrency_semaphore::inactive_read_handle& handle) {
            if (auto reader = _permit.semaphore().unregister_inactive_read(std::move(handle)); reader) {
                _reader = std::move(*reader);
            } else {
                co_await make_reader();
            }
            co_await tick(std::get<flat_mutation_reader>(_reader));
        }

    public:
        reader(schema_ptr s, reader_permit permit, bool memory_only, bool evictable)
            : _schema(std::move(s))
            , _permit(std::move(permit))
            , _memory_only(memory_only)
            , _evictable(evictable)
            , _units(_permit.consume_memory(tests::random::get_int(128, 1024)))
        {
        }
        future<> tick() {
            return std::visit(reader_visitor{*this}, _reader);
        }
        future<> close() noexcept {
            if (auto reader = std::get_if<flat_mutation_reader>(&_reader)) {
                return reader->close();
            }
            return make_ready_future<>();
        }
    };

    const auto count = 10;
    const auto num_readers = 512;
    const auto ticks = 1000;

    simple_schema s;
    reader_concurrency_semaphore semaphore(count, count * 1024, get_name());
    auto stop_sem = deferred_stop(semaphore);

    std::list<std::optional<reader>> readers;
    unsigned nr_memory_only = 0;
    unsigned nr_admitted = 0;
    unsigned nr_evictable = 0;

    for (auto i = 0; i <  num_readers; ++i) {
        const auto memory_only = tests::random::get_bool();
        const auto evictable = !memory_only && tests::random::get_bool();
        if (memory_only) {
            ++nr_memory_only;
        } else if (evictable) {
            ++nr_evictable;
        } else {
            ++nr_admitted;
        }
        readers.emplace_back(reader(s.schema(), semaphore.make_permit(s.schema().get(), fmt::format("reader{}", i)), memory_only, evictable));
    }

    testlog.info("Created {} readers, memory_only={}, admitted={}, evictable={}", readers.size(), nr_memory_only, nr_admitted, nr_evictable);

    bool watchdog_touched = false;
    auto watchdog = timer<db::timeout_clock>([&semaphore, &watchdog_touched] {
        if (!watchdog_touched) {
            testlog.error("Watchdog detected a deadlock, dumping diagnostics before killing the test: {}", semaphore.dump_diagnostics());
            semaphore.broken(std::make_exception_ptr(std::runtime_error("test killed by watchdog")));
        }
        watchdog_touched = false;
    });
    watchdog.arm_periodic(std::chrono::seconds(30));

    parallel_for_each(readers, [&] (std::optional<reader>& r) -> future<> {
        for (auto i = 0; i < ticks; ++i) {
            watchdog_touched = true;
            co_await r->tick();
        }
        co_await r->close();
        r.reset();
        watchdog_touched = true;
    }).get();
}

class dummy_file_impl : public file_impl {
    virtual future<size_t> write_dma(uint64_t pos, const void* buffer, size_t len, const io_priority_class& pc) override {
        return make_ready_future<size_t>(0);
    }

    virtual future<size_t> write_dma(uint64_t pos, std::vector<iovec> iov, const io_priority_class& pc) override {
        return make_ready_future<size_t>(0);
    }

    virtual future<size_t> read_dma(uint64_t pos, void* buffer, size_t len, const io_priority_class& pc) override {
        return make_ready_future<size_t>(0);
    }

    virtual future<size_t> read_dma(uint64_t pos, std::vector<iovec> iov, const io_priority_class& pc) override {
        return make_ready_future<size_t>(0);
    }

    virtual future<> flush(void) override {
        return make_ready_future<>();
    }

    virtual future<struct stat> stat(void) override {
        return make_ready_future<struct stat>();
    }

    virtual future<> truncate(uint64_t length) override {
        return make_ready_future<>();
    }

    virtual future<> discard(uint64_t offset, uint64_t length) override {
        return make_ready_future<>();
    }

    virtual future<> allocate(uint64_t position, uint64_t length) override {
        return make_ready_future<>();
    }

    virtual future<uint64_t> size(void) override {
        return make_ready_future<uint64_t>(0);
    }

    virtual future<> close() override {
        return make_ready_future<>();
    }

    virtual subscription<directory_entry> list_directory(std::function<future<> (directory_entry de)> next) override {
        throw_with_backtrace<std::bad_function_call>();
    }

    virtual future<temporary_buffer<uint8_t>> dma_read_bulk(uint64_t offset, size_t range_size, const io_priority_class& pc) override {
        temporary_buffer<uint8_t> buf(1024);

        memset(buf.get_write(), 0xff, buf.size());

        return make_ready_future<temporary_buffer<uint8_t>>(std::move(buf));
    }
};

SEASTAR_TEST_CASE(reader_restriction_file_tracking) {
    return async([&] {
        reader_concurrency_semaphore semaphore(100, 4 * 1024, get_name());
        auto stop_sem = deferred_stop(semaphore);
        auto permit = semaphore.make_permit(nullptr, get_name());
        permit.wait_admission(0, db::no_timeout).get();

        {
            auto tracked_file = make_tracked_file(file(shared_ptr<file_impl>(make_shared<dummy_file_impl>())), permit);

            BOOST_REQUIRE_EQUAL(4 * 1024, semaphore.available_resources().memory);

            auto buf1 = tracked_file.dma_read_bulk<char>(0, 0).get0();
            BOOST_REQUIRE_EQUAL(3 * 1024, semaphore.available_resources().memory);

            auto buf2 = tracked_file.dma_read_bulk<char>(0, 0).get0();
            BOOST_REQUIRE_EQUAL(2 * 1024, semaphore.available_resources().memory);

            auto buf3 = tracked_file.dma_read_bulk<char>(0, 0).get0();
            BOOST_REQUIRE_EQUAL(1 * 1024, semaphore.available_resources().memory);

            auto buf4 = tracked_file.dma_read_bulk<char>(0, 0).get0();
            BOOST_REQUIRE_EQUAL(0 * 1024, semaphore.available_resources().memory);

            auto buf5 = tracked_file.dma_read_bulk<char>(0, 0).get0();
            BOOST_REQUIRE_EQUAL(-1 * 1024, semaphore.available_resources().memory);

            // Reassing buf1, should still have the same amount of units.
            buf1 = tracked_file.dma_read_bulk<char>(0, 0).get0();
            BOOST_REQUIRE_EQUAL(-1 * 1024, semaphore.available_resources().memory);

            // Move buf1 to the heap, so that we can safely destroy it
            auto buf1_ptr = std::make_unique<temporary_buffer<char>>(std::move(buf1));
            BOOST_REQUIRE_EQUAL(-1 * 1024, semaphore.available_resources().memory);

            buf1_ptr.reset();
            BOOST_REQUIRE_EQUAL(0 * 1024, semaphore.available_resources().memory);

            // Move tracked_file to the heap, so that we can safely destroy it.
            auto tracked_file_ptr = std::make_unique<file>(std::move(tracked_file));
            tracked_file_ptr.reset();

            // Move buf4 to the heap, so that we can safely destroy it
            auto buf4_ptr = std::make_unique<temporary_buffer<char>>(std::move(buf4));
            BOOST_REQUIRE_EQUAL(0 * 1024, semaphore.available_resources().memory);

            // Releasing buffers that overlived the tracked-file they
            // originated from should succeed.
            buf4_ptr.reset();
            BOOST_REQUIRE_EQUAL(1 * 1024, semaphore.available_resources().memory);
        }

        // All units should have been deposited back.
        REQUIRE_EVENTUALLY_EQUAL(4 * 1024, semaphore.available_resources().memory);
    });
}

SEASTAR_TEST_CASE(reader_concurrency_semaphore_timeout) {
    return async([&] () {
        reader_concurrency_semaphore semaphore(2, new_reader_base_cost, get_name());
        auto stop_sem = deferred_stop(semaphore);

        {
            auto timeout = db::timeout_clock::now() + std::chrono::duration_cast<db::timeout_clock::time_point::duration>(std::chrono::milliseconds{1});

            auto permit1 = semaphore.make_permit(nullptr, "permit1");
            std::optional<reader_permit::resource_units> permit1_res = permit1.wait_admission(new_reader_base_cost, timeout).get();

            auto permit2 = semaphore.make_permit(nullptr, "permit2");
            auto permit2_fut = permit2.wait_admission(new_reader_base_cost, timeout);

            auto permit3 = semaphore.make_permit(nullptr, "permit3");
            auto permit3_fut = permit3.wait_admission(new_reader_base_cost, timeout);

            BOOST_REQUIRE_EQUAL(semaphore.waiters(), 2);

            const auto futures_failed = eventually_true([&] { return permit2_fut.failed() && permit3_fut.failed(); });
            BOOST_CHECK(futures_failed);

            if (futures_failed) {
                BOOST_CHECK_THROW(std::rethrow_exception(permit2_fut.get_exception()), semaphore_timed_out);
                BOOST_CHECK_THROW(std::rethrow_exception(permit3_fut.get_exception()), semaphore_timed_out);
            } else {
                // We need special cleanup when the test failed to avoid invalid
                // memory access.
                permit1_res.reset();

                BOOST_CHECK(eventually_true([&] { return permit2_fut.available(); }));
                {
                    auto res = permit2_fut.get();
                }

                BOOST_CHECK(eventually_true([&] { return permit3_fut.available(); }));
                {
                    auto res = permit3_fut.get();
                }
            }
       }

        // All units should have been deposited back.
        REQUIRE_EVENTUALLY_EQUAL(new_reader_base_cost, semaphore.available_resources().memory);
    });
}

SEASTAR_TEST_CASE(reader_concurrency_semaphore_max_queue_length) {
    return async([&] () {
        reader_concurrency_semaphore semaphore(1, new_reader_base_cost, get_name(), 2);
        auto stop_sem = deferred_stop(semaphore);

        {
            auto permit1 = semaphore.make_permit(nullptr, "permit1");
            auto permit1_res = permit1.wait_admission(new_reader_base_cost, db::no_timeout).get();

            auto permit2 = semaphore.make_permit(nullptr, "permit2");
            auto permit2_fut = permit2.wait_admission(new_reader_base_cost, db::no_timeout);

            auto permit3 = semaphore.make_permit(nullptr, "permit3");
            auto permit3_fut = permit3.wait_admission(new_reader_base_cost, db::no_timeout);

            BOOST_REQUIRE_EQUAL(semaphore.waiters(), 2);

            auto permit4 = semaphore.make_permit(nullptr, "permit4");
            auto permit4_fut = permit4.wait_admission(new_reader_base_cost, db::no_timeout);

            // The queue should now be full.
            BOOST_REQUIRE_THROW(permit4_fut.get(), std::runtime_error);

            permit1_res.reset();
            {
                auto res = permit2_fut.get0();
            }
            {
                auto res = permit3_fut.get();
            }
        }

        REQUIRE_EVENTUALLY_EQUAL(new_reader_base_cost, semaphore.available_resources().memory);
    });
}

SEASTAR_THREAD_TEST_CASE(reader_concurrency_semaphore_dump_reader_diganostics) {
    reader_concurrency_semaphore semaphore(reader_concurrency_semaphore::no_limits{}, get_name());
    auto stop_sem = deferred_stop(semaphore);

    const auto nr_tables = tests::random::get_int<unsigned>(2, 4);
    std::vector<schema_ptr> schemas;
    for (unsigned i = 0; i < nr_tables; ++i) {
        schemas.emplace_back(schema_builder("ks", fmt::format("tbl{}", i))
                .with_column("pk", int32_type, column_kind::partition_key)
                .with_column("v", int32_type, column_kind::regular_column).build());
    }

    const auto nr_ops = tests::random::get_int<unsigned>(1, 3);
    std::vector<std::string> op_names;
    for (unsigned i = 0; i < nr_ops; ++i) {
        op_names.emplace_back(fmt::format("op{}", i));
    }

    std::deque<std::pair<reader_permit, reader_permit::resource_units>> permits;
    for (auto& schema : schemas) {
        const auto nr_permits = tests::random::get_int<unsigned>(2, 32);
        for (unsigned i = 0; i < nr_permits; ++i) {
            auto permit = semaphore.make_permit(schema.get(), op_names.at(tests::random::get_int<unsigned>(0, nr_ops - 1)));
            if (tests::random::get_int<unsigned>(0, 4)) {
                auto units = permit.consume_resources(reader_resources(tests::random::get_int<unsigned>(0, 1), tests::random::get_int<unsigned>(1024, 16 * 1024 * 1024)));
                permits.push_back(std::pair(std::move(permit), std::move(units)));
            } else {
                auto hdnl = semaphore.register_inactive_read(make_empty_flat_reader(schema, permit));
                BOOST_REQUIRE(semaphore.try_evict_one_inactive_read());
                auto units = permit.consume_memory(tests::random::get_int<unsigned>(1024, 2048));
                permits.push_back(std::pair(std::move(permit), std::move(units)));
            }
        }
    }

    testlog.info("With max-lines=4: {}", semaphore.dump_diagnostics(4));
    testlog.info("With no max-lines: {}", semaphore.dump_diagnostics(0));
}
