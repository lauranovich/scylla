/*
 * Copyright 2019-present ScyllaDB
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
 * You should have received a copy of the GNU Affero General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <vector>
#include <string>
#include <variant>

#include <seastar/core/shared_ptr.hh>

#include "utils/rjson.hh"

/*
 * Parsed representation of expressions and their components.
 *
 * Types in alternator::parse namespace are used for holding the parse
 * tree - objects generated by the Antlr rules after parsing an expression.
 * Because of the way Antlr works, all these objects are default-constructed
 * first, and then assigned when the rule is completed, so all these types
 * have only default constructors - but setter functions to set them later.
 */

namespace alternator {
namespace parsed {

// "path" is an attribute's path in a document, e.g., a.b[3].c.
class path {
    // All paths have a "root", a top-level attribute, and any number of
    // "dereference operators" - each either an index (e.g., "[2]") or a
    // dot (e.g., ".xyz").
    std::string _root;
    std::vector<std::variant<std::string, unsigned>> _operators;
    // It is useful to limit the depth of a user-specified path, because is
    // allows us to use recursive algorithms without worrying about recursion
    // depth. DynamoDB officially limits the length of paths to 32 components
    // (including the root) so let's use the same limit.
    static constexpr unsigned depth_limit = 32;
    void check_depth_limit();
public:
    void set_root(std::string root) {
        _root = std::move(root);
    }
    void add_index(unsigned i) {
        _operators.emplace_back(i);
        check_depth_limit();
    }
    void add_dot(std::string(name)) {
        _operators.emplace_back(std::move(name));
        check_depth_limit();
    }
    const std::string& root() const {
        return _root;
    }
    bool has_operators() const {
        return !_operators.empty();
    }
    const std::vector<std::variant<std::string, unsigned>>& operators() const {
        return _operators;
    }
    std::vector<std::variant<std::string, unsigned>>& operators() {
        return _operators;
    }
    friend std::ostream& operator<<(std::ostream&, const path&);
};

// When an expression is first parsed, all constants are references, like
// ":val1", into ExpressionAttributeValues. This uses std::string() variant.
// The resolve_value() function replaces these constants by the JSON item
// extracted from the ExpressionAttributeValues.
struct constant {
    // We use lw_shared_ptr<rjson::value> just to make rjson::value copyable,
    // to make this entire object copyable as ANTLR needs.
    using literal = lw_shared_ptr<rjson::value>;
    std::variant<std::string, literal> _value;
    void set(const rjson::value& v) {
        _value = make_lw_shared<rjson::value>(rjson::copy(v));
    }
    void set(std::string& s) {
        _value = s;
    }
};

// "value" is is a value used in the right hand side of an assignment
// expression, "SET a = ...". It can be a constant (a reference to a value
// included in the request, e.g., ":val"), a path to an attribute from the
// existing item (e.g., "a.b[3].c"), or a function of other such values.
// Note that the real right-hand-side of an assignment is actually a bit
// more general - it allows either a value, or a value+value or value-value -
// see class set_rhs below.
struct value {
    struct function_call {
        std::string _function_name;
        std::vector<value> _parameters;
    };
    std::variant<constant, path, function_call> _value;
    void set_constant(constant c) {
        _value = std::move(c);
    }
    void set_valref(std::string s) {
        _value = constant { std::move(s) };
    }
    void set_path(path p) {
        _value = std::move(p);
    }
    void set_func_name(std::string s) {
        _value = function_call {std::move(s), {}};
    }
    void add_func_parameter(value v) {
        std::get<function_call>(_value)._parameters.emplace_back(std::move(v));
    }
    bool is_constant() const {
        return std::holds_alternative<constant>(_value);
    }
    bool is_path() const {
        return std::holds_alternative<path>(_value);
    }
    bool is_func() const {
        return std::holds_alternative<function_call>(_value);
    }
};

// The right-hand-side of a SET in an update expression can be either a
// single value (see above), or value+value, or value-value.
class set_rhs {
public:
    char _op;  // '+', '-', or 'v''
    value _v1;
    value _v2;
    void set_value(value&& v1) {
        _op = 'v';
        _v1 = std::move(v1);
    }
    void set_plus(value&& v2) {
        _op = '+';
        _v2 = std::move(v2);
    }
    void set_minus(value&& v2) {
        _op = '-';
        _v2 = std::move(v2);
    }
};

class update_expression {
public:
    struct action {
        path _path;
        struct set {
            set_rhs _rhs;
        };
        struct remove {
        };
        struct add {
            constant _valref;
        };
        struct del {
            constant _valref;
        };
        std::variant<set, remove, add, del> _action;

        void assign_set(path p, set_rhs rhs) {
            _path = std::move(p);
            _action = set { std::move(rhs) };
        }
        void assign_remove(path p) {
            _path = std::move(p);
            _action = remove { };
        }
        void assign_add(path p, std::string v) {
            _path = std::move(p);
            _action = add { constant { std::move(v) } };
        }
        void assign_del(path p, std::string v) {
            _path = std::move(p);
            _action = del { constant { std::move(v) } };
        }
    };
private:
    std::vector<action> _actions;
    bool seen_set = false;
    bool seen_remove = false;
    bool seen_add = false;
    bool seen_del = false;
public:
    void add(action a);
    void append(update_expression other);
    bool empty() const {
        return _actions.empty();
    }
    const std::vector<action>& actions() const {
        return _actions;
    }
    std::vector<action>& actions() {
        return _actions;
    }
};

// A primitive_condition is a condition expression involving one condition,
// while the full condition_expression below adds boolean logic over these
// primitive conditions.
// The supported primitive conditions are:
// 1. Binary operators - v1 OP v2, where OP is =, <>, <, <=, >, or >= and
//    v1 and v2 are values - from the item (an attribute path), the query
//    (a ":val" reference), or a function of the the above (only the size()
//    function is supported).
// 2. Ternary operator - v1 BETWEEN v2 and v3 (means v1 >= v2 AND v1 <= v3).
// 3. N-ary operator - v1 IN ( v2, v3, ... )
// 4. A single function call (attribute_exists etc.). The parser actually
//    accepts a more general "value" here but later stages reject a value
//    which is not a function call (because DynamoDB does it too).
class primitive_condition {
public:
    enum class type {
        UNDEFINED, VALUE, EQ, NE, LT, LE, GT, GE, BETWEEN, IN
    };
    type _op = type::UNDEFINED;
    std::vector<value> _values;
    void set_operator(type op) {
        _op = op;
    }
    void add_value(value&& v) {
        _values.push_back(std::move(v));
    }
    bool empty() const {
        return _op == type::UNDEFINED;
    }
};

class condition_expression {
public:
    bool _negated = false; // If true, the entire condition is negated
    struct condition_list {
        char op = '|'; // '&' or '|'
        std::vector<condition_expression> conditions;
    };
    std::variant<primitive_condition, condition_list> _expression = condition_list();

    void set_primitive(primitive_condition&& p) {
        _expression = std::move(p);
    }
    void append(condition_expression&& c, char op);
    void apply_not() {
        _negated = !_negated;
    }
    bool empty() const {
        return std::holds_alternative<condition_list>(_expression) &&
               std::get<condition_list>(_expression).conditions.empty();
    }
};

} // namespace parsed
} // namespace alternator
