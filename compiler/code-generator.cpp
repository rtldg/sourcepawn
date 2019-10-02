// vim: set ts=8 sts=4 sw=4 tw=99 et:
//  Pawn compiler - Recursive descend expresion parser
//
//  Copyright (c) ITB CompuPhase, 1997-2005
//
//  This software is provided "as-is", without any express or implied warranty.
//  In no event will the authors be held liable for any damages arising from
//  the use of this software.
//
//  Permission is granted to anyone to use this software for any purpose,
//  including commercial applications, and to alter it and redistribute it
//  freely, subject to the following restrictions:
//
//  1.  The origin of this software must not be misrepresented; you must not
//      claim that you wrote the original software. If you use this software in
//      a product, an acknowledgment in the product documentation would be
//      appreciated but is not required.
//  2.  Altered source versions must be plainly marked as such, and must not be
//      misrepresented as being the original software.
//  3.  This notice may not be removed or altered from any source distribution.
//
//  Version: $Id$

#include "emitter.h"
#include "errors.h"
#include "expressions.h"
#include "sctracker.h"

void
Expr::Emit()
{
    AutoErrorPos aep(pos_);

    if (val_.ident == iCONSTEXPR) {
        ldconst(val_.constval, sPRI);
        return;
    }
    DoEmit();
}

void
Expr::EmitTest(bool jump_on_true, int taken, int /* fallthrough */)
{
    Emit();
    if (jump_on_true)
        jmp_ne0(taken);
    else
        jmp_eq0(taken);
}

void
IsDefinedExpr::DoEmit()
{
    // Always constant.
    assert(false);
}

void
UnaryExpr::DoEmit()
{
    expr_->Emit();

    // Hack: abort early if the operation was already handled. We really just
    // want to replace the UnaryExpr though.
    if (userop_)
        return;

    switch (token_) {
        case '~':
            invert();
            break;
        case '!':
            lneg();
            break;
        case '-':
            neg();
            break;
        default:
            assert(false);
    }
}

void
PreIncExpr::DoEmit()
{
    expr_->Emit();

    const auto& val = expr_->val();
    value tmp = val;

    if (val.ident != iACCESSOR) {
        if (userop_.sym) {
            emit_userop(userop_, &tmp);
        } else {
            if (token_ == tINC)
                inc(&tmp); /* increase variable first */
            else
                dec(&tmp);
        }
        rvalue(&tmp);  /* and read the result into PRI */
    } else {
        pushreg(sPRI);
        invoke_getter(val.accessor);
        if (userop_.sym) {
            emit_userop(userop_, &tmp);
        } else {
            if (token_ == tINC)
                inc_pri();
            else
                dec_pri();
        }
        popreg(sALT);
        invoke_setter(val.accessor, TRUE);
    }
}

void
PostIncExpr::DoEmit()
{
    expr_->Emit();

    const auto& val = expr_->val();
    value tmp = val;

    if (val.ident != iACCESSOR) {
        /* on incrementing array cells, the address in PRI must be saved for
         * incremening the value, whereas the current value must be in PRI
         * on exit.
         */
        int saveresult = (val.ident == iARRAYCELL || val.ident == iARRAYCHAR);
        if (saveresult)
            pushreg(sPRI); /* save address in PRI */
        rvalue(&tmp);      /* read current value into PRI */
        if (saveresult)
            swap1(); /* save PRI on the stack, restore address in PRI */
        if (userop_.sym) {
            emit_userop(userop_, &tmp);
        } else {
            if (token_ == tINC)
                inc(&tmp);
            else
                dec(&tmp);
        }
        if (saveresult)
            popreg(sPRI); /* restore PRI (result of rvalue()) */
    } else {
        pushreg(sPRI); // save obj
        invoke_getter(val.accessor);
        move_alt();    // alt = oldval
        swap1();       // pri = saved obj, stack = [oldval]
        pushreg(sPRI); // pri = obj, alt = oldval, stack = [obj, oldval]
        moveto1();     // pri = oldval, stack = [obj, oldval]

        if (userop_.sym) {
            emit_userop(userop_, &tmp);
        } else {
            if (token_ == tINC)
                inc_pri();
            else
                dec_pri();
        }

        popreg(sALT);
        invoke_setter(val.accessor, FALSE);
        popreg(sPRI);
    }
}

void
BinaryExpr::DoEmit()
{
    if (IsChainedOp(token_)) {
        EmitChainedCompare();
        return;
    }

    // We emit constexprs in the |oper_| handler below.
    const auto& left_val = left_->val();
    if (IsAssignOp(token_) || left_val.ident != iCONSTEXPR)
        left_->Emit();

    bool saved_lhs = false;
    if (IsAssignOp(token_)) {
        switch (left_val.ident) {
            case iARRAYCELL:
            case iARRAYCHAR:
            case iARRAY:
            case iREFARRAY:
                if (oper_) {
                    pushreg(sPRI);
                    rvalue(left_val);
                    saved_lhs = true;
                }
                break;
            case iACCESSOR:
                pushreg(sPRI);
                if (oper_)
                    rvalue(left_val);
                saved_lhs = true;
                break;
            default:
                assert(left_->lvalue());
                if (oper_)
                    rvalue(left_val);
                break;
        }

        if (array_copy_length_) {
            assert(!oper_);
            assert(!assignop_.sym);

            pushreg(sPRI);
            right_->Emit();
            popreg(sALT);
            memcopy(array_copy_length_ * sizeof(cell));
            return;
        }
    }

    assert(!array_copy_length_);
    assert(left_val.ident != iARRAY && left_val.ident != iREFARRAY);

    EmitInner(this, left_, right_);

    if (IsAssignOp(token_)) {
        if (saved_lhs)
            popreg(sALT);

        auto tmp = left_val;
        if (assignop_.sym)
            emit_userop(assignop_, nullptr);
        store(&tmp);
    }
}

void
BinaryExpr::EmitChainedCompare()
{
    auto exprs = FlattenChainedCompares(this);

    Expr* left = exprs.back()->left();
    if (left->val().ident != iCONSTEXPR)
        left->Emit();

    int count = 0;
    while (!exprs.empty()) {
        BinaryExpr* root = exprs.popBackCopy();
        Expr* right = root->right();

        // EmitInner() guarantees the right-hand side will be preserved in ALT.
        // emit_userop implicitly guarantees this, as do os_less etc which
        // use XCHG to swap the LHS/RHS expressions.
        if (count)
            relop_prefix();
        EmitInner(root, left, right);
        if (count)
            relop_suffix();

        left = right;
        count++;
    }
}

void
BinaryExpr::EmitInner(BinaryExpr* root, Expr* left, Expr* right)
{
    const auto& left_val = left->val();
    const auto& right_val = right->val();

    // left goes into ALT, right goes into PRI, though we can swap them for
    // commutative operations.
    auto oper = root->oper();
    if (left_val.ident == iCONSTEXPR) {
        if (right_val.ident == iCONSTEXPR)
            ldconst(right_val.constval, sPRI);
        else
            right->Emit();
        ldconst(left_val.constval, sALT);
    } else {
        // If performing a binary operation, we need to make sure the LHS winds
        // up in ALT. If performing a store, we only need to preserve LHS to
        // ALT if it can't be re-evaluated.
        bool must_save_lhs = oper || !left_val.canRematerialize();
        if (right_val.ident == iCONSTEXPR) {
            if (commutative(oper)) {
                ldconst(right_val.constval, sALT);
            } else {
                if (must_save_lhs)
                    pushreg(sPRI);
                ldconst(right_val.constval, sPRI);
                if (must_save_lhs)
                    popreg(sALT);
            }
        } else {
            if (must_save_lhs)
                pushreg(sPRI);
            right->Emit();
            if (must_save_lhs)
                popreg(sALT);
        }
    }

    if (oper) {
        const auto& userop = root->userop();
        if (userop.sym)
            emit_userop(userop, nullptr);
        else
            oper();
    }
}

void
LogicalExpr::DoEmit()
{
    int done = getlabel();
    int taken = getlabel();
    int fallthrough = getlabel();

    EmitTest(true, taken, fallthrough);
    setlabel(fallthrough);
    ldconst(0, sPRI);
    jumplabel(done);
    setlabel(taken);
    ldconst(1, sPRI);
    setlabel(done);
}

void
LogicalExpr::EmitTest(bool jump_on_true, int taken, int fallthrough)
{
    ke::Vector<Expr*> sequence;
    FlattenLogical(token_, &sequence);

    // a || b || c .... given jumpOnTrue, should be:
    //
    //   resolve a
    //   jtrue TAKEN
    //   resolve b
    //   jtrue TAKEN
    //   resolve c
    //   jtrue TAKEN
    //
    // a || b || c .... given jumpOnFalse, should be:
    //   resolve a
    //   jtrue FALLTHROUGH
    //   resolve b
    //   jtrue FALLTHROUGH
    //   resolve c
    //   jfalse TAKEN
    //  FALLTHROUGH:
    //
    // a && b && c ..... given jumpOnTrue, should be:
    //   resolve a
    //   jfalse FALLTHROUGH
    //   resolve b
    //   jfalse FALLTHROUGH
    //   resolve c
    //   jtrue TAKEN
    //  FALLTHROUGH:
    //
    // a && b && c ..... given jumpOnFalse, should be:
    //   resolve a
    //   jfalse TAKEN
    //   resolve b
    //   jfalse TAKEN
    //   resolve c
    //   jfalse TAKEN
    //
    // This is fairly efficient, and by re-entering test() we can ensure each
    // jfalse/jtrue encodes things like "a > b" with a combined jump+compare
    // instruction.
    //
    // Note: to make this slightly easier to read, we make all this logic
    // explicit below rather than collapsing it into a single test() call.
    for (size_t i = 0; i < sequence.length() - 1; i++) {
        Expr* expr = sequence[i];
        if (token_ == tlOR) {
            if (jump_on_true)
                expr->EmitTest(true, taken, fallthrough);
            else
                expr->EmitTest(true, fallthrough, taken);
        } else {
            assert(token_ == tlAND);
            if (jump_on_true)
                expr->EmitTest(false, fallthrough, taken);
            else
                expr->EmitTest(false, taken, fallthrough);
        }
    }

    Expr* last = sequence.back();
    last->EmitTest(jump_on_true, taken, fallthrough);
}

void
TernaryExpr::DoEmit()
{
    first_->Emit();

    int flab1 = getlabel();
    int flab2 = getlabel();
    cell_t total1 = 0;
    cell_t total2 = 0;

    pushheaplist();
    jmp_eq0(flab1); /* go to second expression if primary register==0 */

    second_->Emit();

    if ((total1 = pop_static_heaplist())) {
        setheap_save(total1 * sizeof(cell));
    }
    pushheaplist();
    jumplabel(flab2);
    setlabel(flab1);

    third_->Emit();

    if ((total2 = pop_static_heaplist())) {
        setheap_save(total2 * sizeof(cell));
    }
    setlabel(flab2);
    if (val_.ident == iREFARRAY && (total1 && total2)) {
        markheap(MEMUSE_DYNAMIC, 0);
    }
}

void
CastExpr::DoEmit()
{
    expr_->Emit();
}

void
SymbolExpr::DoEmit()
{
    switch (sym_->ident) {
        case iCONSTEXPR:
            ldconst(sym_->addr(), sPRI);
            break;
        case iARRAY:
        case iREFARRAY:
            address(sym_, sPRI);
            break;
        case iFUNCTN:
            load_glbfn(sym_);
            markusage(sym_, uCALLBACK);
            break;
        case iVARIABLE:
        case iREFERENCE:
            break;
        default:
            assert(false);
    }
}

void
RvalueExpr::DoEmit()
{
    expr_->Emit();

    value val = expr_->val();
    rvalue(&val);
}

void
CommaExpr::DoEmit()
{
    for (const auto& expr : exprs_)
        expr->Emit();
}

void
ArrayExpr::DoEmit()
{
    ldconst(addr_, sPRI);
}

void
ThisExpr::DoEmit()
{
    if (sym_->ident == iREFARRAY)
        address(sym_, sPRI);
}

void
NullExpr::DoEmit()
{
    // Always const.
    assert(false);
}

void
NumberExpr::DoEmit()
{
    // Always const.
    assert(false);
}

void
FloatExpr::DoEmit()
{
    // Always const.
    assert(false);
}

void
StringExpr::DoEmit()
{
    ldconst(lit_addr_, sPRI);
}

void
IndexExpr::DoEmit()
{
    base_->Emit();

    symbol* sym = base_->val().sym;
    assert(sym);

    bool magic_string = (sym->tag == pc_tag_string && sym->dim.array.level == 0);

    const auto& idxval = expr_->val();
    if (idxval.ident == iCONSTEXPR) {
        if (!(sym->tag == pc_tag_string && sym->dim.array.level == 0)) {
            /* normal array index */
            if (idxval.constval != 0) {
                /* don't add offsets for zero subscripts */
                ldconst(idxval.constval << 2, sALT);
                ob_add();
            }
        } else {
            /* character index */
            if (idxval.constval != 0) {
                /* don't add offsets for zero subscripts */
                ldconst(idxval.constval, sALT); /* 8-bit character */
                ob_add();
            }
        }
    } else {
        pushreg(sPRI);
        expr_->Emit();

        /* array index is not constant */
        if (!magic_string) {
            if (sym->dim.array.length != 0)
                ffbounds(sym->dim.array.length - 1); /* run time check for array bounds */
            else
                ffbounds();
            cell2addr(); /* normal array index */
        } else {
            if (sym->dim.array.length != 0)
                ffbounds(sym->dim.array.length * (32 / sCHARBITS) - 1);
            else
                ffbounds();
            char2addr(); /* character array index */
        }
        popreg(sALT);
        ob_add(); /* base address was popped into secondary register */
    }

    /* the indexed item may be another array (multi-dimensional arrays) */
    if (sym->dim.array.level > 0) {
        /* read the offset to the subarray and add it to the current address */
        value val = base_->val();
        val.ident = iARRAYCELL;
        pushreg(sPRI); /* the optimizer makes this to a MOVE.alt */
        rvalue(&val);
        popreg(sALT);
        ob_add();
    }
}

void
FieldAccessExpr::DoEmit()
{
    assert(token_ == '.');

    // Note that we do not load an iACCESSOR here, we only make sure the base
    // is computed. Emit() never performs loads on l-values, that ability is
    // reserved for RvalueExpr().
    base_->Emit();

    if (field_ && field_->addr()) {
        ldconst(field_->addr() << 2, sALT);
        ob_add();
    }
}

void
SizeofExpr::DoEmit()
{
    // Always a constant.
    assert(false);
}

void
CallExpr::DoEmit()
{
    // If returning an array, push a hidden parameter.
    if (val_.sym) {
        int retsize = array_totalsize(val_.sym);
        assert(retsize > 0  || !cc_ok());

        modheap(retsize * sizeof(cell));
        pushreg(sALT);
        markheap(MEMUSE_STATIC, retsize);
    }

    // Everything heap-allocated after here is owned by the callee.
    pushheaplist();

    for (size_t i = argv_.length() - 1; i < argv_.length(); i--) {
        const auto& expr = argv_[i].expr;
        const auto& arg = argv_[i].arg;

        expr->Emit();

        if (expr->AsDefaultArgExpr()) {
            pushreg(sPRI);
            continue;
        }

        const auto& val = expr->val();
        bool lvalue = expr->lvalue();

        switch (arg->ident) {
            case iVARARGS:
                if (val.ident == iVARIABLE || val.ident == iREFERENCE) {
                    assert(val.sym);
                    assert(lvalue);
                    /* treat a "const" variable passed to a function with a non-const
                     * "variable argument list" as a constant here */
                    if ((val.sym->usage & uCONST) && !(arg->usage & uCONST)) {
                        rvalue(val);
                        setheap_pri();
                    } else if (lvalue) {
                        address(val.sym, sPRI);
                    } else {
                        setheap_pri();
                    }
                } else if (val.ident == iCONSTEXPR || val.ident == iEXPRESSION) {
                    /* allocate a cell on the heap and store the
                     * value (already in PRI) there */
                    setheap_pri();
                }
                if (val.sym)
                    markusage(val.sym, uWRITTEN);
                break;
            case iVARIABLE:
            case iREFARRAY:
                break;
            case iREFERENCE:
                if (val.ident == iVARIABLE || val.ident == iREFERENCE) {
                    assert(val.sym);
                    address(val.sym, sPRI);
                }
                if (val.sym)
                    markusage(val.sym, uWRITTEN);
                break;
            default:
                assert(false);
                break;
        }

        pushreg(sPRI);
        markexpr(sPARM, NULL, 0); // mark the end of a sub-expression
    }

    ffcall(sym_, argv_.length());

    if (val_.sym)
        popreg(sPRI); // Pop hidden parameter as function result

    // Scrap all temporary heap allocations used to perform the call.
    popheaplist(true);
}

void
DefaultArgExpr::DoEmit()
{
    switch (arg_->ident) {
        case iREFARRAY:
        {
            auto& def = arg_->defvalue.array;
            bool is_const = (arg_->usage & uCONST) != 0;

            setdefarray(def.data, def.size, def.arraysize, &def.addr, is_const);
            if (def.data)
                assert(arg_->numdim > 0);
            break;
        }
        case iREFERENCE:
            setheap(arg_->defvalue.val);
            markheap(MEMUSE_STATIC, 1);
            break;
        case iVARIABLE:
            ldconst(arg_->defvalue.val, sPRI);
            break;
        default:
            assert(false);
    }

}

void
CallUserOpExpr::DoEmit()
{
    expr_->Emit();

    if (userop_.oper) {
        auto val = expr_->val();
        emit_userop(userop_, &val);
    } else {
        emit_userop(userop_, nullptr);
    }
}