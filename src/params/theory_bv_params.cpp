/*++
Copyright (c) 2012 Microsoft Corporation

Module Name:

    theory_bv_params.cpp

Abstract:

    <abstract>

Author:

    Leonardo de Moura (leonardo) 2012-12-02.

Revision History:

--*/
#include "params/theory_bv_params.h"
#include "params/smt_params_helper.hpp"
#include "params/bv_rewriter_params.hpp"

void theory_bv_params::updt_params(params_ref const & _p) {
    smt_params_helper p(_p);
    bv_rewriter_params rp(_p);
    m_hi_div0 = rp.hi_div0();
    m_bv_reflect = p.bv_reflect();
    m_bv_enable_int2bv2int = p.bv_enable_int2bv(); 
    m_bv_delay = p.bv_delay();
    m_bv_size_reduce = p.bv_size_reduce();
    m_bv_solver = p.bv_solver();
}

#define DISPLAY_PARAM(X) out << #X"=" << X << '\n';

void theory_bv_params::display(std::ostream & out) const {
    DISPLAY_PARAM(m_bv_mode);
    DISPLAY_PARAM(m_hi_div0);
    DISPLAY_PARAM(m_bv_reflect);
    DISPLAY_PARAM(m_bv_lazy_le);
    DISPLAY_PARAM(m_bv_cc);
    DISPLAY_PARAM(m_bv_blast_max_size);
    DISPLAY_PARAM(m_bv_enable_int2bv2int);
    DISPLAY_PARAM(m_bv_delay);
    DISPLAY_PARAM(m_bv_size_reduce);
    DISPLAY_PARAM(m_bv_solver);
}
