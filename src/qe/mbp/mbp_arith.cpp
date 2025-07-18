/*++
Copyright (c) 2015 Microsoft Corporation

Module Name:

    qe_arith.cpp

Abstract:

    Simple projection function for real arithmetic based on Loos-W.

Author:

    Nikolaj Bjorner (nbjorner) 2013-09-12

Revision History:

    Moved projection functionality to model_based_opt module. 2016-06-26

--*/

#include "qe/mbp/mbp_arith.h"
#include "ast/ast_util.h"
#include "ast/arith_decl_plugin.h"
#include "ast/ast_pp.h"
#include "ast/expr_functors.h"
#include "ast/rewriter/expr_safe_replace.h"
#include "math/simplex/model_based_opt.h"
#include "model/model_evaluator.h"
#include "model/model_smt2_pp.h"
#include "model/model_v2_pp.h"

namespace mbp {

    struct arith_project_plugin::imp {

        ast_manager& m;
        arith_util        a;
        bool              m_check_purified = true;  // check that variables are properly pure 
        bool              m_apply_projection = false;

        imp(ast_manager& m) :
            m(m), a(m) {}

        void insert_mul(expr* x, rational const& v, obj_map<expr, rational>& ts) {
            rational w;
            if (ts.find(x, w))
                ts.insert(x, w + v);
            else
                ts.insert(x, v);
        }


        //
        // extract linear inequalities from literal 'lit' into the model-based optimization manager 'mbo'.
        // It uses the current model to choose values for conditionals and it primes mbo with the current
        // interpretation of sub-expressions that are treated as variables for mbo.
        // 
        bool linearize(opt::model_based_opt& mbo, model_evaluator& eval, expr* lit, expr_ref_vector& fmls, obj_map<expr, unsigned>& tids) {
            obj_map<expr, rational> ts;
            rational c(0), mul(1);
            expr_ref t(m);
            opt::ineq_type ty = opt::t_le;
            expr* e1, * e2;
            DEBUG_CODE(expr_ref val(m);
            eval(lit, val);
            CTRACE(qe, !m.is_true(val), tout << mk_pp(lit, m) << " := " << val << "\n";);
            if (m.is_false(val))
                return false;
            SASSERT(m.limit().is_canceled() || !m.is_false(val)););

            if (!m.inc())
                return false;

            TRACE(opt, tout << mk_pp(lit, m) << " " << a.is_lt(lit) << " " << a.is_gt(lit) << "\n";);
            bool is_not = m.is_not(lit, lit);
            if (is_not) {
                mul.neg();
            }
            SASSERT(!m.is_not(lit));
            if ((a.is_le(lit, e1, e2) || a.is_ge(lit, e2, e1))) {
                linearize(mbo, eval, mul, e1, c, fmls, ts, tids);
                linearize(mbo, eval, -mul, e2, c, fmls, ts, tids);
                ty = is_not ? opt::t_lt : opt::t_le;
            }
            else if ((a.is_lt(lit, e1, e2) || a.is_gt(lit, e2, e1))) {
                linearize(mbo, eval, mul, e1, c, fmls, ts, tids);
                linearize(mbo, eval, -mul, e2, c, fmls, ts, tids);
                ty = is_not ? opt::t_le : opt::t_lt;
            }
            else if (m.is_eq(lit, e1, e2) && !is_not && is_arith(e1)) {
                linearize(mbo, eval, mul, e1, c, fmls, ts, tids);
                linearize(mbo, eval, -mul, e2, c, fmls, ts, tids);
                ty = opt::t_eq;
            }
            else if (m.is_eq(lit, e1, e2) && is_not && is_arith(e1)) {

                rational r1, r2;
                expr_ref val1 = eval(e1);
                expr_ref val2 = eval(e2);
                if (!a.is_numeral(val1, r1)) return false;
                if (!a.is_numeral(val2, r2)) return false;
                SASSERT(r1 != r2);
                if (r1 < r2) {
                    std::swap(e1, e2);
                }
                ty = opt::t_lt;
                linearize(mbo, eval, mul, e1, c, fmls, ts, tids);
                linearize(mbo, eval, -mul, e2, c, fmls, ts, tids);
            }
            else if (m.is_distinct(lit) && !is_not && is_arith(to_app(lit)->get_arg(0))) {
                expr_ref val(m);
                rational r;
                app* alit = to_app(lit);
                vector<std::pair<expr*, rational> > nums;
                for (expr* arg : *alit) {
                    val = eval(arg);
                    TRACE(qe, tout << mk_pp(arg, m) << " " << val << "\n";);
                    if (!a.is_numeral(val, r)) return false;
                    nums.push_back(std::make_pair(arg, r));
                }
                std::sort(nums.begin(), nums.end(), compare_second());
                for (unsigned i = 0; i + 1 < nums.size(); ++i) {
                    SASSERT(nums[i].second < nums[i + 1].second);
                    expr_ref fml(a.mk_lt(nums[i].first, nums[i + 1].first), m);
                    if (!linearize(mbo, eval, fml, fmls, tids)) {
                        return false;
                    }
                }
                return true;
            }
            else if (m.is_distinct(lit) && is_not && is_arith(to_app(lit)->get_arg(0))) {
                // find the two arguments that are equal.
                // linearize these.                
                map<rational, expr*, rational::hash_proc, rational::eq_proc> values;
                bool found_eq = false;
                for (unsigned i = 0; !found_eq && i < to_app(lit)->get_num_args(); ++i) {
                    expr* arg1 = to_app(lit)->get_arg(i), * arg2 = nullptr;
                    rational r;
                    expr_ref val = eval(arg1);
                    TRACE(qe, tout << mk_pp(arg1, m) << " " << val << "\n";);
                    if (!a.is_numeral(val, r)) return false;
                    if (values.find(r, arg2)) {
                        ty = opt::t_eq;
                        linearize(mbo, eval, mul, arg1, c, fmls, ts, tids);
                        linearize(mbo, eval, -mul, arg2, c, fmls, ts, tids);
                        found_eq = true;
                    }
                    else {
                        values.insert(r, arg1);
                    }
                }
                SASSERT(found_eq);
            }
            else if (m.is_and(lit) && !is_not) {
                fmls.append(to_app(lit)->get_num_args(), to_app(lit)->get_args());
                return true;
            }
            else if (m.is_or(lit) && is_not) {
                for (expr* arg : *to_app(lit))
                    fmls.push_back(mk_not(m, arg));
                return true;
            }
            else if (m.is_or(lit) && !is_not) {
                for (expr* arg : *to_app(lit)) {
                    if (eval.is_true(arg)) {
                        fmls.push_back(arg);
                        return true;
                    }
                }
                TRACE(qe, tout << "Skipping " << mk_pp(lit, m) << "\n";);
                return false;
            }
            else if (m.is_and(lit) && is_not) {
                for (expr* arg : *to_app(lit)) {
                    if (eval.is_false(arg)) {
                        fmls.push_back(mk_not(m, arg));
                        return true;
                    }
                }
                TRACE(qe, tout << "Skipping " << mk_pp(lit, m) << "\n";);
                return false;
            }
            else {
                TRACE(qe, tout << "Skipping " << mk_pp(lit, m) << "\n";);
                return false;
            }
            vars coeffs;
            extract_coefficients(mbo, eval, ts, tids, coeffs);
            mbo.add_constraint(coeffs, c, ty);
            return true;
        }

        //
        // convert linear arithmetic term into an inequality for mbo.
        // 
        void linearize(opt::model_based_opt& mbo, model_evaluator& eval, rational const& mul, expr* t, rational& c,
            expr_ref_vector& fmls, obj_map<expr, rational>& ts, obj_map<expr, unsigned>& tids) {
            expr* t1, * t2, * t3;
            rational mul1;
            expr_ref val(m);

            auto add_def = [&](expr* t1, rational const& m, vars& coeffs) {
                obj_map<expr, rational> ts0;
                rational mul0(1), c0(0);
                linearize(mbo, eval, mul0, t1, c0, fmls, ts0, tids);
                extract_coefficients(mbo, eval, ts0, tids, coeffs);
                insert_mul(t, mul, ts);
                return c0;
            };

            if (tids.contains(t))
                insert_mul(t, mul, ts);
            else if (a.is_mul(t, t1, t2) && is_numeral(t1, mul1))
                linearize(mbo, eval, mul * mul1, t2, c, fmls, ts, tids);
            else if (a.is_mul(t, t1, t2) && is_numeral(t2, mul1))
                linearize(mbo, eval, mul * mul1, t1, c, fmls, ts, tids);
            else if (a.is_uminus(t, t1))
                linearize(mbo, eval, -mul, t1, c, fmls, ts, tids);
            else if (a.is_numeral(t, mul1))
                c += mul * mul1;
            else if (a.is_add(t)) {
                for (expr* arg : *to_app(t))
                    linearize(mbo, eval, mul, arg, c, fmls, ts, tids);
            }
            else if (a.is_sub(t, t1, t2)) {
                linearize(mbo, eval, mul, t1, c, fmls, ts, tids);
                linearize(mbo, eval, -mul, t2, c, fmls, ts, tids);
            }

            else if (m.is_ite(t, t1, t2, t3)) {
                val = eval(t1);
                TRACE(qe, tout << mk_pp(t1, m) << " := " << val << "\n";);
                if (m.is_true(val)) {
                    linearize(mbo, eval, mul, t2, c, fmls, ts, tids);
                    fmls.push_back(t1);
                }
                else if (m.is_false(val)) {
                    expr_ref not_t1(mk_not(m, t1), m);
                    fmls.push_back(not_t1);
                    linearize(mbo, eval, mul, t3, c, fmls, ts, tids);
                }
                else {
                    IF_VERBOSE(1, verbose_stream() << "mbp failed on if: " << mk_pp(t, m) << " := " << val << "\n");
                    throw default_exception("mbp evaluation didn't produce a truth value");
                }
            }
            else if (a.is_mod(t, t1, t2) && is_numeral(t2, mul1) && mul1 > 0) {
                // v = t1 mod mul1
                vars coeffs;
                rational c0 = add_def(t1, mul1, coeffs);
                tids.insert(t, mbo.add_mod(coeffs, c0, mul1));

            }
            else if (a.is_idiv(t, t1, t2) && is_numeral(t2, mul1) && mul1 > 0) {
                // v = t1 div mul1
                vars coeffs;
                rational c0 = add_def(t1, mul1, coeffs);
                tids.insert(t, mbo.add_div(coeffs, c0, mul1));
            }
            else if (a.is_mod(t, t1, t2) && is_numeral(t2, mul1) && mul1 > 0) {
                rational r;
                val = eval(t);
                if (!a.is_numeral(val, r)) {
                    IF_VERBOSE(1, verbose_stream() << "mbp failed on " << mk_pp(t, m) << " := " << val << "\n");
                    throw default_exception("mbp evaluation didn't produce an integer");
                }
                c += mul * r;

                rational c0(-r), mul0(1);
                obj_map<expr, rational> ts0;
                linearize(mbo, eval, mul0, t1, c0, fmls, ts0, tids);
                vars coeffs;
                extract_coefficients(mbo, eval, ts0, tids, coeffs);
                mbo.add_divides(coeffs, c0, mul1);
            }
            else {
                TRACE(qe, tout << "insert mul " << mk_pp(t, m) << "\n");
                insert_mul(t, mul, ts);
            }
        }

        bool is_numeral(expr* t, rational& r) {
            return a.is_extended_numeral(t, r);
        }

        struct compare_second {
            bool operator()(std::pair<expr*, rational> const& a,
                std::pair<expr*, rational> const& b) const {
                return a.second < b.second;
            }
        };

        bool is_arith(expr* e) {
            return a.is_int_real(e);
        }

        rational n_sign(rational const& b) {
            return rational(b.is_pos() ? -1 : 1);
        }

        bool project1(model& model, app* v, app_ref_vector& vars, expr_ref_vector& lits) {
            app_ref_vector vs(m);
            vs.push_back(v);
            vector<def> defs;
            return project(model, vs, lits, defs, false) && vs.empty();
        }

        typedef opt::model_based_opt::var var;
        typedef opt::model_based_opt::row row;
        typedef vector<var> vars;

        expr_ref var2expr(ptr_vector<expr> const& index2expr, var const& v) {
            expr_ref t(index2expr[v.m_id], m);
            if (!v.m_coeff.is_one()) 
                t = a.mk_mul(a.mk_numeral(v.m_coeff, a.is_int(t)), t);            
            return t;
        }

        bool project(model& model, app_ref_vector& vars, expr_ref_vector& fmls, vector<def>& result, bool compute_def) {
            bool has_arith = false;
            for (expr* v : vars)
                has_arith |= is_arith(v);
            if (!has_arith)
                return true;
            model_evaluator eval(model);
            TRACE(qe, tout << model;);
            eval.set_model_completion(true);
            model.set_inline();
            compute_def |= m_apply_projection;

            opt::model_based_opt mbo;
            obj_map<expr, unsigned> tids;
            expr_ref_vector pinned(m);
            unsigned j = 0;
            TRACE(qe, tout << "vars: " << vars << "\n";
            for (expr* f : fmls) tout << mk_pp(f, m) << " := " << model(f) << "\n";);
            for (unsigned i = 0; i < fmls.size(); ++i) {
                expr* fml = fmls.get(i);
                if (!linearize(mbo, eval, fml, fmls, tids)) {
                    TRACE(qe, tout << "could not linearize: " << mk_pp(fml, m) << "\n";);
                    fmls[j++] = fml;
                }
                else {
                    pinned.push_back(fml);
                }
            }
            fmls.shrink(j);
            TRACE(qe, tout << "formulas\n" << fmls << "\n";
            for (auto const& [e, id] : tids)
                tout << mk_pp(e, m) << " -> " << id << "\n";);

            // fmls holds residue,
            // mbo holds linear inequalities that are in scope
            // collect variables in residue an in tids.
            // filter variables that are absent from residue.
            // project those.
            // collect result of projection
            // return those to fmls.

            expr_mark var_mark, fmls_mark;
            for (app* v : vars) {
                var_mark.mark(v);
                if (is_arith(v) && !tids.contains(v)) {
                    rational r;
                    expr_ref val = eval(v);
                    if (!m.inc())
                        return false;
                    if (!a.is_numeral(val, r)) {
                        IF_VERBOSE(1, verbose_stream() << "mbp failed on " << mk_pp(v, m) << " := " << val << "\n");
                        throw default_exception("evaluation did not produce a numeral");
                    }
                    TRACE(qe, tout << mk_pp(v, m) << " " << val << "\n";);
                    tids.insert(v, mbo.add_var(r, a.is_int(v)));
                }
            }

            // bail on variables in non-linear sub-terms
            auto is_pure = [&](expr* e) {
                expr* x, * y;
                rational r;
                if (a.is_mod(e, x, y) && a.is_numeral(y))
                    return true;
                if (a.is_idiv(e, x, y) && a.is_numeral(y, r) && r > 0)
                    return true;
                return false;
            };

            for (auto& [e, v] : tids) {
                if (is_arith(e) && !is_pure(e) && !var_mark.is_marked(e))
                    mark_rec(fmls_mark, e);
            }
            if (m_check_purified) {
                for (expr* fml : fmls)
                    mark_rec(fmls_mark, fml);
                for (auto& kv : tids) {
                    expr* e = kv.m_key;
                    if (!var_mark.is_marked(e) && !is_pure(e))
                        mark_rec(fmls_mark, e);
                }
            }

            ptr_vector<expr> index2expr;
            for (auto& kv : tids)
                index2expr.setx(kv.m_value, kv.m_key, nullptr);

            j = 0;
            unsigned_vector real_vars;
            for (app* v : vars) {
                if (is_arith(v) && !fmls_mark.is_marked(v))
                    real_vars.push_back(tids.find(v));
                else
                    vars[j++] = v;
            }
            vars.shrink(j);

            TRACE(qe, tout << "remaining vars: " << vars << "\n";
            for (unsigned v : real_vars) tout << "v" << v << " " << mk_pp(index2expr[v], m) << "\n";
            mbo.display(tout););
            vector<opt::model_based_opt::def_ref> defs = mbo.project(real_vars.size(), real_vars.data(), compute_def);


            vector<row> rows;
            u_map<row> def_vars;
            mbo.get_live_rows(rows);
            for (row const& r : rows) {
                if (r.m_type == opt::t_mod)
                    def_vars.insert(r.m_id, r);
                else if (r.m_type == opt::t_div)
                    def_vars.insert(r.m_id, r);
            }
            rows2fmls(def_vars, rows, index2expr, fmls);
            TRACE(qe, mbo.display(tout << "mbo result\n");
            for (auto const& d : defs) if (d) tout << "def: " << *d << "\n";
            tout << fmls << "\n";);

            if (compute_def)
                optdefs2mbpdef(def_vars, defs, index2expr, real_vars, result);
            if (m_apply_projection && !apply_projection(eval, result, fmls))
                return false;

            TRACE(qe,
                for (auto const& [v, t] : result)
                    tout << v << " := " << t << "\n";
                  for (auto* f : fmls)
                      tout << mk_pp(f, m) << " := " << eval(f) << "\n";
                  tout << "fmls:" << fmls << "\n";);
            return true;
        }

        expr_ref from_def(u_map<row> const& def_vars, opt::model_based_opt::def const& d, bool is_int, ptr_vector<expr> const& index2expr) {
            if (d.is_add()) {
                return expr_ref(
                    a.mk_add(from_def(def_vars, *d.to_add().x, is_int, index2expr),
                             from_def(def_vars, *d.to_add().y, is_int, index2expr)), m); 

            }
            if (d.is_mul()) {
                return expr_ref(
                    a.mk_mul(from_def(def_vars, *d.to_mul().x, is_int, index2expr),
                        from_def(def_vars, *d.to_mul().y, is_int, index2expr)), m);
            }
            if (d.is_const()) 
                return expr_ref(a.mk_numeral(d.to_const().c, is_int), m);
            if (d.is_var()) {
                auto t = id2expr(def_vars, index2expr, d.to_var().v.m_id);
                if (d.to_var().v.m_coeff != 1)
                    t = a.mk_mul(a.mk_numeral(d.to_var().v.m_coeff, is_int), t);
                return expr_ref(t, m);
            }
            if (d.is_div()) {
                auto t = from_def(def_vars, *d.to_div().x, is_int, index2expr);
                if (is_int)
                    t = a.mk_idiv(t, a.mk_numeral(d.to_div().m_div, is_int));
                else
                    t = a.mk_div(t, a.mk_numeral(d.to_div().m_div, is_int));
                return expr_ref(t, m);
            }
            UNREACHABLE();
            return expr_ref(nullptr, m);
        }

        void optdefs2mbpdef(u_map<row> const& def_vars, vector<opt::model_based_opt::def_ref> const& defs, ptr_vector<expr> const& index2expr, unsigned_vector const& real_vars, vector<def>& result) {
            SASSERT(defs.size() == real_vars.size());
            for (unsigned i = 0; i < defs.size(); ++i) {
                auto const& d = defs[i];
                expr* x = index2expr[real_vars[i]];
                bool is_int = a.is_int(x);
                auto t = from_def(def_vars, *d, is_int, index2expr);
                result.push_back({ expr_ref(x, m), t });
            }
        }

        expr_ref id2expr(u_map<row> const& def_vars, ptr_vector<expr> const& index2expr, unsigned id) {
            row r;
            if (def_vars.find(id, r))
                return row2expr(def_vars, index2expr, r);
            return expr_ref(index2expr[id], m);
        }

        expr_ref row2expr(u_map<row> const& def_vars, ptr_vector<expr> const& index2expr, row const& r) {
            expr_ref_vector ts(m);
            expr_ref t(m);
            rational n;
            for (var const& v : r.m_vars) {
                t = id2expr(def_vars, index2expr, v.m_id);
                if (a.is_numeral(t, n) && n == 0)
                    continue;
                else if (a.is_numeral(t, n))
                    t = a.mk_numeral(v.m_coeff * n, a.is_int(t));
                else if (!v.m_coeff.is_one())
                    t = a.mk_mul(a.mk_numeral(v.m_coeff, a.is_int(t)), t);
                ts.push_back(t);
            }
            switch (r.m_type) {
            case opt::t_mod:
                if (ts.empty()) {
                    t = a.mk_int(mod(r.m_coeff, r.m_mod));
                    return t;
                }
                if (r.m_coeff != 0)
                    ts.push_back(a.mk_int(r.m_coeff));
                t = mk_add(ts);
                t = a.mk_mod(t, a.mk_int(r.m_mod));
                return t;
            case opt::t_div:
                if (ts.empty()) {
                    t = a.mk_int(div(r.m_coeff, r.m_mod));
                    return t;
                }
                if (r.m_coeff != 0)
                    ts.push_back(a.mk_int(r.m_coeff));
                t = mk_add(ts);
                t = a.mk_idiv(t, a.mk_int(r.m_mod));
                return t;
            case opt::t_divides:
                ts.push_back(a.mk_int(r.m_coeff));
                return mk_add(ts);
            default:
                return mk_add(ts);
            }
        }

        void rows2fmls(u_map<row>& def_vars, vector<row> const& rows, ptr_vector<expr> const& index2expr, expr_ref_vector& fmls) {

            for (row const& r : rows) {
                expr_ref t(m), s(m), val(m);

                if (r.m_vars.empty())
                    continue;
                if (r.m_type == opt::t_mod)
                    continue;
                if (r.m_type == opt::t_div)
                    continue;

                if (r.m_vars.size() == 1 && r.m_vars[0].m_coeff.is_neg() && 
                    (r.m_type == opt::t_eq || r.m_type == opt::t_le || r.m_type == opt::t_lt)) {
                    var const& v = r.m_vars[0];
                    t = id2expr(def_vars, index2expr, v.m_id);
                    if (!v.m_coeff.is_minus_one()) {
                        t = a.mk_mul(a.mk_numeral(-v.m_coeff, a.is_int(t)), t);
                    }
                    s = a.mk_numeral(r.m_coeff, a.is_int(t));
                    switch (r.m_type) {
                    case opt::t_lt: t = a.mk_gt(t, s); break;
                    case opt::t_le: t = a.mk_ge(t, s); break;
                    case opt::t_eq: t = a.mk_eq(t, s); break;
                    default: UNREACHABLE(); break;
                    }
                    fmls.push_back(t);
                    continue;
                }
                t = row2expr(def_vars, index2expr, r);
                s = a.mk_numeral(-r.m_coeff, r.m_coeff.is_int() && a.is_int(t));
                switch (r.m_type) {
                case opt::t_lt: t = a.mk_lt(t, s); break;
                case opt::t_le: t = a.mk_le(t, s); break;
                case opt::t_eq: t = a.mk_eq(t, s); break;
                case opt::t_divides:
                    t = a.mk_eq(a.mk_mod(t, a.mk_int(r.m_mod)), a.mk_int(0));
                    break;
                default:
                    UNREACHABLE();
                    break;
                }
                fmls.push_back(t);
            }
        }

        expr_ref mk_add(expr_ref_vector const& ts) {
            return a.mk_add_simplify(ts);
        }

        opt::inf_eps maximize(expr_ref_vector const& fmls0, model& mdl, app* t, expr_ref& ge, expr_ref& gt) {
            SASSERT(a.is_real(t));
            expr_ref_vector fmls(fmls0);
            opt::model_based_opt mbo;
            opt::inf_eps value;
            obj_map<expr, rational> ts;
            obj_map<expr, unsigned> tids;
            model_evaluator eval(mdl);
            // extract objective function.
            vars coeffs;
            rational c(0), mul(1);
            linearize(mbo, eval, mul, t, c, fmls, ts, tids);
            extract_coefficients(mbo, eval, ts, tids, coeffs);
            mbo.set_objective(coeffs, c);

            SASSERT(validate_model(eval, fmls0));

            // extract linear constraints

            for (expr* fml : fmls) {
                linearize(mbo, eval, fml, fmls, tids);
            }

            // find optimal value
            value = mbo.maximize();


            // update model to use new values that satisfy optimality
            ptr_vector<expr> vars;
            for (auto& kv : tids) {
                expr* e = kv.m_key;
                if (is_uninterp_const(e)) {
                    unsigned id = kv.m_value;
                    func_decl* f = to_app(e)->get_decl();
                    expr_ref val(a.mk_numeral(mbo.get_value(id), false), m);
                    mdl.register_decl(f, val);
                }
                else {
                    TRACE(qe, tout << "omitting model update for non-uninterpreted constant " << mk_pp(e, m) << "\n";);
                }
            }
            expr_ref val(a.mk_numeral(value.get_rational(), false), m);
            expr_ref tval = eval(t);

            // update the predicate 'bound' which forces larger values when 'strict' is true.
            // strict:  bound := valuue < t
            // !strict: bound := value <= t
            if (!value.is_finite()) {
                ge = a.mk_ge(t, tval);
                gt = m.mk_false();
            }
            else if (value.get_infinitesimal().is_neg()) {
                ge = a.mk_ge(t, tval);
                gt = a.mk_ge(t, val);
            }
            else {
                ge = a.mk_ge(t, val);
                gt = a.mk_gt(t, val);
            }
            SASSERT(validate_model(eval, fmls0));
            return value;
        }

        bool validate_model(model_evaluator& eval, expr_ref_vector const& fmls) {
            bool valid = true;
            for (expr* fml : fmls) {
                expr_ref val = eval(fml);
                if (!m.is_true(val)) {
                    valid = false;
                    TRACE(qe, tout << mk_pp(fml, m) << " := " << val << "\n";);
                }
            }
            return valid;
        }

        void extract_coefficients(opt::model_based_opt& mbo, model_evaluator& eval, obj_map<expr, rational> const& ts, obj_map<expr, unsigned>& tids, vars& coeffs) {
            coeffs.reset();
            eval.set_model_completion(true);
            for (auto& kv : ts) {
                unsigned id;
                expr* v = kv.m_key;
                if (!tids.find(v, id)) {
                    rational r;
                    expr_ref val = eval(v);
                    if (!a.is_numeral(val, r)) {
                        TRACE(qe, tout << eval.get_model() << "\n";);
                        IF_VERBOSE(1, verbose_stream() << "mbp failed on " << mk_pp(v, m) << " := " << val << "\n");
                        throw default_exception("mbp evaluation was only partial");
                    }
                    id = mbo.add_var(r, a.is_int(v));
                    tids.insert(v, id);
                }
                CTRACE(qe, kv.m_value.is_zero(), tout << mk_pp(v, m) << " has coefficient 0\n";);
                if (!kv.m_value.is_zero()) {
                    coeffs.push_back(var(id, kv.m_value));
                }
            }
        }

        bool apply_projection(model_evaluator& eval, vector<def> const& defs, expr_ref_vector& fmls) {
            if (fmls.empty() || defs.empty())
                return true;
            expr_safe_replace subst(m);
            expr_ref_vector fmls_tmp(m);
            expr_ref tmp(m);
            for (unsigned i = defs.size(); i-- > 0; ) {
                auto const& d = defs[i];
                subst(d.term, tmp);
                subst.insert(d.var, tmp);
            }
            unsigned j = 0;
            for (expr* fml : fmls) {
                subst(fml, tmp);
                if (m.is_false(eval(tmp)))
                    return false;
                fmls[j++] = tmp;
            }
            return true;
        }

    };

    arith_project_plugin::arith_project_plugin(ast_manager& m) :project_plugin(m) {
        m_imp = alloc(imp, m);
    }

    arith_project_plugin::~arith_project_plugin() {
        dealloc(m_imp);
    }

    bool arith_project_plugin::project1(model& model, app* var, app_ref_vector& vars, expr_ref_vector& lits) {
        return m_imp->project1(model, var, vars, lits);
    }

    bool arith_project_plugin::project(model& model, app_ref_vector& vars, expr_ref_vector& lits) {
        vector<def> defs;
        return m_imp->project(model, vars, lits, defs, false);
    }

    bool arith_project_plugin::project(model& model, app_ref_vector& vars, expr_ref_vector& lits, vector<def>& defs) {
        return m_imp->project(model, vars, lits, defs, true);
    }

    void arith_project_plugin::set_check_purified(bool check_purified) {
        m_imp->m_check_purified = check_purified;
    }

    void arith_project_plugin::set_apply_projection(bool f) {
        m_imp->m_apply_projection = f;
    }

    family_id arith_project_plugin::get_family_id() {
        return m_imp->a.get_family_id();
    }

    opt::inf_eps arith_project_plugin::maximize(expr_ref_vector const& fmls, model& mdl, app* t, expr_ref& ge, expr_ref& gt) {
        return m_imp->maximize(fmls, mdl, t, ge, gt);
    }

    bool arith_project(model& model, app* var, expr_ref_vector& lits) {
        ast_manager& m = lits.get_manager();
        arith_project_plugin ap(m);
        app_ref_vector vars(m);
        return ap.project1(model, var, vars, lits);
    }
}
