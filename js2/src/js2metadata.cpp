/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
*
* The contents of this file are subject to the Netscape Public
* License Version 1.1 (the "License"); you may not use this file
* except in compliance with the License. You may obtain a copy of
* the License at http://www.mozilla.org/NPL/
*
* Software distributed under the License is distributed on an "AS
* IS" basis, WITHOUT WARRANTY OF ANY KIND, either express or
* implied. See the License for the specific language governing
* rights and limitations under the License.
*
* The Original Code is the JavaScript 2 Prototype.
*
* The Initial Developer of the Original Code is Netscape
* Communications Corporation.   Portions created by Netscape are
* Copyright (C) 1998 Netscape Communications Corporation. All
* Rights Reserved.
*
* Contributor(s):
*
* Alternatively, the contents of this file may be used under the
* terms of the GNU Public License (the "GPL"), in which case the
* provisions of the GPL are applicable instead of those above.
* If you wish to allow use of your version of this file only
* under the terms of the GPL and not to allow others to use your
* version of this file under the NPL, indicate your decision by
* deleting the provisions above and replace them with the notice
* and other provisions required by the GPL.  If you do not delete
* the provisions above, a recipient may use your version of this
* file under either the NPL or the GPL.
*/


#ifdef _WIN32
#include "msvc_pragma.h"
#endif


#include <algorithm>
#include <assert.h>
#include <map>
#include <list>
#include <stack>

#include "world.h"
#include "utilities.h"
#include "js2value.h"
#include "jslong.h"
#include "numerics.h"
#include "reader.h"
#include "parser.h"
#include "regexp.h"
#include "js2engine.h"
#include "bytecodecontainer.h"
#include "js2metadata.h"


namespace JavaScript {
namespace MetaData {



/************************************************************************************
 *
 *  Statements and statement lists
 *
 ************************************************************************************/


    /*
     * Validate the linked list of statement nodes beginning at 'p'
     */
    void JS2Metadata::ValidateStmtList(StmtNode *p) {
        while (p) {
            ValidateStmt(&cxt, env, Singular, p);
            p = p->next;
        }
    }

    void JS2Metadata::ValidateStmtList(Context *cxt, Environment *env, Plurality pl, StmtNode *p) {
        while (p) {
            ValidateStmt(cxt, env, pl, p);
            p = p->next;
        }
    }
     
    JS2Object *JS2Metadata::validateStaticFunction(FunctionDefinition *fnDef, js2val compileThis, bool prototype, bool unchecked, Context *cxt, Environment *env)
    {
        ParameterFrame *compileFrame = new ParameterFrame(compileThis, prototype);
        JS2Object *result;
        
        if (prototype) {
            FunctionInstance *fInst = new FunctionInstance(this, functionClass->prototype, functionClass);
            fInst->fWrap = new FunctionWrapper(unchecked, compileFrame, env);
            fnDef->fWrap = fInst->fWrap;
            result = fInst;
        }
        else {
            SimpleInstance *sInst = new SimpleInstance(functionClass);
            sInst->fWrap = new FunctionWrapper(unchecked, compileFrame, env);
            fnDef->fWrap = sInst->fWrap;
            result = sInst;
        }

        RootKeeper rk(&result);
        Frame *curTopFrame = env->getTopFrame();
        CompilationData *oldData = startCompilationUnit(fnDef->fWrap->bCon, bCon->mSource, bCon->mSourceLocation);
        try {
            env->addFrame(compileFrame);
            VariableBinding *pb = fnDef->parameters;
            if (pb) {
                NamespaceList publicNamespaceList;
                publicNamespaceList.push_back(publicNamespace);
                uint32 pCount = 0;
                while (pb) {
                    pCount++;
                    pb = pb->next;
                }
                if (prototype)
                    checked_cast<PrototypeInstance *>(result)->writeProperty(this, engine->length_StringAtom, INT_TO_JS2VAL(pCount), DynamicPropertyValue::READONLY);
                pb = fnDef->parameters;
                compileFrame->positional = new Variable *[pCount];
                compileFrame->positionalCount = pCount;
                pCount = 0;
                while (pb) {
                    // XXX define a static binding for each parameter
                    Variable *v = new Variable(objectClass, JS2VAL_UNDEFINED, false);
                    compileFrame->positional[pCount++] = v;
                    pb->mn = defineLocalMember(env, pb->name, &publicNamespaceList, Attribute::NoOverride, false, ReadWriteAccess, v, pb->pos);
                    pb = pb->next;
                }
            }
            ValidateStmt(cxt, env, Plural, fnDef->body);
            env->removeTopFrame();
        }
        catch (Exception x) {
            restoreCompilationUnit(oldData);
            env->setTopFrame(curTopFrame);
            throw x;
        }
        restoreCompilationUnit(oldData);
        return result;
    }

    /*
     * Validate an individual statement 'p', including it's children
     */
    void JS2Metadata::ValidateStmt(Context *cxt, Environment *env, Plurality pl, StmtNode *p) 
    {
        CompoundAttribute *a = NULL;
        RootKeeper rk(&a);
        Frame *curTopFrame = env->getTopFrame();

        try {
            switch (p->getKind()) {
            case StmtNode::block:
            case StmtNode::group:
                {
                    BlockStmtNode *b = checked_cast<BlockStmtNode *>(p);
                    b->compileFrame = new BlockFrame();
                    bCon->saveFrame(b->compileFrame);   // stash this frame so it doesn't get gc'd before eval pass.
                    env->addFrame(b->compileFrame);
                    ValidateStmtList(cxt, env, pl, b->statements);
                    env->removeTopFrame();
                }
                break;
            case StmtNode::label:
                {
                    LabelStmtNode *l = checked_cast<LabelStmtNode *>(p);
                    l->labelID = bCon->getLabel();
    /*
        A labelled statement catches contained, named, 'breaks' but simply adds itself as a label for
        contained iteration statements. (i.e. you can 'break' out of a labelled statement, but not 'continue'
        one, however the statement label becomes a 'continuable' label for all contained iteration statements.
    */
                    // Make sure there is no existing break target with the same name
                    for (TargetListIterator si = targetList.begin(), end = targetList.end(); (si != end); si++) {
                        switch ((*si)->getKind()) {
                        case StmtNode::label:
                            if (checked_cast<LabelStmtNode *>(*si)->name == l->name)
                                reportError(Exception::syntaxError, "Duplicate statement label", p->pos);
                            break;
                        }
                    }
                    targetList.push_back(p);
                    ValidateStmt(cxt, env, pl, l->stmt);
                    targetList.pop_back();
                }
                break;
            case StmtNode::If:
                {
                    UnaryStmtNode *i = checked_cast<UnaryStmtNode *>(p);
                    ValidateExpression(cxt, env, i->expr);
                    ValidateStmt(cxt, env, pl, i->stmt);
                }
                break;
            case StmtNode::IfElse:
                {
                    BinaryStmtNode *i = checked_cast<BinaryStmtNode *>(p);
                    ValidateExpression(cxt, env, i->expr);
                    ValidateStmt(cxt, env, pl, i->stmt);
                    ValidateStmt(cxt, env, pl, i->stmt2);
                }
                break;
            case StmtNode::ForIn:
                // XXX need to validate that first expression is a single binding ?
            case StmtNode::For:
                {
                    ForStmtNode *f = checked_cast<ForStmtNode *>(p);
                    f->breakLabelID = bCon->getLabel();
                    f->continueLabelID = bCon->getLabel();
                    if (f->initializer)
                        ValidateStmt(cxt, env, pl, f->initializer);
                    if (f->expr2)
                        ValidateExpression(cxt, env, f->expr2);
                    if (f->expr3)
                        ValidateExpression(cxt, env, f->expr3);
                    f->breakLabelID = bCon->getLabel();
                    f->continueLabelID = bCon->getLabel();
                    targetList.push_back(p);
                    ValidateStmt(cxt, env, pl, f->stmt);
                    targetList.pop_back();
                }
                break;
            case StmtNode::Switch:
                {
                    SwitchStmtNode *sw = checked_cast<SwitchStmtNode *>(p);
                    sw->breakLabelID = bCon->getLabel();
                    ValidateExpression(cxt, env, sw->expr);
                    targetList.push_back(p);
                    StmtNode *s = sw->statements;
                    while (s) {
                        if (s->getKind() == StmtNode::Case) {
                            ExprStmtNode *c = checked_cast<ExprStmtNode *>(s);
                            c->labelID = bCon->getLabel();
                            if (c->expr)
                                ValidateExpression(cxt, env, c->expr);
                        }
                        else
                            ValidateStmt(cxt, env, pl, s);
                        s = s->next;
                    }
                    targetList.pop_back();
                }
                break;
            case StmtNode::While:
            case StmtNode::DoWhile:
                {
                    UnaryStmtNode *w = checked_cast<UnaryStmtNode *>(p);
                    w->breakLabelID = bCon->getLabel();
                    w->continueLabelID = bCon->getLabel();
                    targetList.push_back(p);
                    ValidateExpression(cxt, env, w->expr);
                    ValidateStmt(cxt, env, pl, w->stmt);
                    targetList.pop_back();
                }
                break;
            case StmtNode::Break:
                {
                    GoStmtNode *g = checked_cast<GoStmtNode *>(p);
                    bool found = false;
                    for (TargetListReverseIterator si = targetList.rbegin(), end = targetList.rend(); (si != end); si++) {
                        if (g->name) {
                            // Make sure the name is on the targetList as a viable break target...
                            // (only label statements can introduce names)
                            if ((*si)->getKind() == StmtNode::label) {
                                LabelStmtNode *l = checked_cast<LabelStmtNode *>(*si);
                                if (l->name == *g->name) {
                                    g->tgtID = l->labelID;
                                    found = true;
                                    break;
                                }
                            }
                        }
                        else {
                            // anything at all will do
                            switch ((*si)->getKind()) {
                            case StmtNode::label:
                                {
                                    LabelStmtNode *l = checked_cast<LabelStmtNode *>(*si);
                                    g->tgtID = l->labelID;
                                }
                                break;
                            case StmtNode::While:
                            case StmtNode::DoWhile:
                                {
                                    UnaryStmtNode *w = checked_cast<UnaryStmtNode *>(*si);
                                    g->tgtID = w->breakLabelID;
                                }
                                break;
                            case StmtNode::For:
                            case StmtNode::ForIn:
                                {
                                    ForStmtNode *f = checked_cast<ForStmtNode *>(*si);
                                    g->tgtID = f->breakLabelID;
                                }
                                break;
                           case StmtNode::Switch:
                                {
                                    SwitchStmtNode *s = checked_cast<SwitchStmtNode *>(*si);
                                    g->tgtID = s->breakLabelID;
                                }
                                break;
                            }
                            found = true;
                            break;
                        }
                    }
                    if (!found) 
                        reportError(Exception::syntaxError, "No such break target available", p->pos);
                }
                break;
            case StmtNode::Continue:
                {
                    GoStmtNode *g = checked_cast<GoStmtNode *>(p);
                    bool found = false;
                    for (TargetListIterator si = targetList.begin(), end = targetList.end(); (si != end); si++) {
                        if (g->name) {
                            // Make sure the name is on the targetList as a viable continue target...
                            if ((*si)->getKind() == StmtNode::label) {
                                LabelStmtNode *l = checked_cast<LabelStmtNode *>(*si);
                                if (l->name == *g->name) {
                                    g->tgtID = l->labelID;
                                    found = true;
                                    break;
                                }
                            }
                        }
                        else {
                            // only some non-label statements will do
                            switch ((*si)->getKind()) {
                            case StmtNode::While:
                            case StmtNode::DoWhile:
                                {
                                    UnaryStmtNode *w = checked_cast<UnaryStmtNode *>(*si);
                                    g->tgtID = w->continueLabelID;
                                }
                                break;
                            case StmtNode::For:
                            case StmtNode::ForIn:
                                {
                                    ForStmtNode *f = checked_cast<ForStmtNode *>(*si);
                                    g->tgtID = f->continueLabelID;
                                }
                            }
                            found = true;
                            break;
                        }
                    }
                    if (!found) 
                        reportError(Exception::syntaxError, "No such break target available", p->pos);
                }
                break;
            case StmtNode::Throw:
                {
                    ExprStmtNode *e = checked_cast<ExprStmtNode *>(p);
                    ValidateExpression(cxt, env, e->expr);
                }
                break;
            case StmtNode::Try:
                {
                    TryStmtNode *t = checked_cast<TryStmtNode *>(p);
                    ValidateStmt(cxt, env, pl, t->stmt);
                    if (t->finally)
                        ValidateStmt(cxt, env, pl, t->finally);
                    CatchClause *c = t->catches;
                    while (c) {                    
                        ValidateStmt(cxt, env, pl, c->stmt);
                        if (c->type)
                            ValidateExpression(cxt, env, c->type);
                        c = c->next;
                    }
                }
                break;
            case StmtNode::Return:
                {
                    ExprStmtNode *e = checked_cast<ExprStmtNode *>(p);
                    if (e->expr) {
                        ValidateExpression(cxt, env, e->expr);
                    }
                }
                break;
            case StmtNode::Function:
                {
                    Attribute *attr = NULL;
                    FunctionStmtNode *f = checked_cast<FunctionStmtNode *>(p);
                    if (f->attributes) {
                        ValidateAttributeExpression(cxt, env, f->attributes);
                        attr = EvalAttributeExpression(env, CompilePhase, f->attributes);
                    }
                    a = Attribute::toCompoundAttribute(attr);
                    if (a->dynamic)
                        reportError(Exception::definitionError, "Illegal attribute", p->pos);
                    VariableBinding *vb = f->function.parameters;
                    bool untyped = (f->function.resultType == NULL);
                    if (untyped) {
                        while (vb) {
                            if (vb->type) {
                                untyped = false;
                                break;
                            }
                            vb = vb->next;
                        }
                    }
                    bool unchecked = !cxt->strict && (env->getTopFrame()->kind != ClassKind)
                                        && (f->function.prefix == FunctionName::normal) && untyped;
                    bool prototype = unchecked || a->prototype;
                    Attribute::MemberModifier memberMod = a->memberMod;
                    if (env->getTopFrame()->kind == ClassKind) {
                        if (memberMod == Attribute::NoModifier)
                            memberMod = Attribute::Virtual;
                    }
                    else {
                        if (memberMod != Attribute::NoModifier)
                            reportError(Exception::definitionError, "Illegal attribute", p->pos);
                    }
                    if (prototype && ((f->function.prefix != FunctionName::normal) || (memberMod == Attribute::Constructor))) {
                        reportError(Exception::definitionError, "Illegal attribute", p->pos);
                    }
                    js2val compileThis = JS2VAL_VOID;
                    if (prototype || (memberMod == Attribute::Constructor) 
                                  || (memberMod == Attribute::Virtual) 
                                  || (memberMod == Attribute::Final))
                        compileThis = JS2VAL_INACCESSIBLE;
                    Frame *topFrame = env->getTopFrame();

                    switch (memberMod) {
                    case Attribute::NoModifier:
                    case Attribute::Static:
                        {
                            if (f->function.prefix != FunctionName::normal) {
                                // XXX getter/setter --> ????
                            }
                            else {
                                JS2Object *fObj = validateStaticFunction(&f->function, compileThis, prototype, unchecked, cxt, env);
                                if (unchecked 
                                        && (f->attributes == NULL)
                                        && ((topFrame->kind == GlobalObjectKind)
                                                        || (topFrame->kind == BlockKind)
                                                        || (topFrame->kind == ParameterKind)) ) {
                                    DynamicVariable *v = defineHoistedVar(env, f->function.name, p);
                                    v->value = OBJECT_TO_JS2VAL(fObj);
                                }
                                else {
                                    Variable *v = new Variable(functionClass, OBJECT_TO_JS2VAL(fObj), true);
                                    defineLocalMember(env, f->function.name, a->namespaces, a->overrideMod, a->xplicit, ReadWriteAccess, v, p->pos);
                                }
                            }
                        }
                        break;
                    case Attribute::Virtual:
                    case Attribute::Final:
                        {
                    // XXX Here the spec. has ???, so the following is tentative
                            JS2Object *fObj = validateStaticFunction(&f->function, compileThis, prototype, unchecked, cxt, env);
                            JS2Class *c = checked_cast<JS2Class *>(env->getTopFrame());
                            InstanceMember *m = new InstanceMethod(checked_cast<SimpleInstance *>(fObj));
                            defineInstanceMember(c, cxt, f->function.name, a->namespaces, a->overrideMod, a->xplicit, ReadWriteAccess, m, p->pos);
                        }
                        break;
                    case Attribute::Constructor:
                        {
                    // XXX Here the spec. has ???, so the following is tentative
                            ASSERT(!prototype); // XXX right?
                            JS2Object *fObj = validateStaticFunction(&f->function, compileThis, prototype, unchecked, cxt, env);
                            ConstructorMethod *cm = new ConstructorMethod(OBJECT_TO_JS2VAL(fObj));
                            defineLocalMember(env, f->function.name, a->namespaces, a->overrideMod, a->xplicit, ReadWriteAccess, cm, p->pos);
                        }
                        break;
                    }

                }
                break;
            case StmtNode::Var:
            case StmtNode::Const:
                {
                    bool immutable = (p->getKind() == StmtNode::Const);
                    Attribute *attr = NULL;
                    VariableStmtNode *vs = checked_cast<VariableStmtNode *>(p);

                    if (vs->attributes) {
                        ValidateAttributeExpression(cxt, env, vs->attributes);
                        attr = EvalAttributeExpression(env, CompilePhase, vs->attributes);
                    }
                
                    VariableBinding *vb = vs->bindings;
                    Frame *regionalFrame = *(env->getRegionalFrame());
                    while (vb)  {
                        const StringAtom *name = vb->name;
                        if (vb->type)
                            ValidateTypeExpression(cxt, env, vb->type);
                        vb->member = NULL;
                        if (vb->initializer)
                            ValidateExpression(cxt, env, vb->initializer);

                        if (!cxt->strict && ((regionalFrame->kind == GlobalObjectKind)
                                            || (regionalFrame->kind == ParameterKind))
                                        && !immutable
                                        && (vs->attributes == NULL)
                                        && (vb->type == NULL)) {
                            defineHoistedVar(env, name, p);
                        }
                        else {
                            a = Attribute::toCompoundAttribute(attr);
                            if (a->dynamic || a->prototype)
                                reportError(Exception::definitionError, "Illegal attribute", p->pos);
                            Attribute::MemberModifier memberMod = a->memberMod;
                            if ((env->getTopFrame()->kind == ClassKind)
                                    && (memberMod == Attribute::NoModifier))
                                memberMod = Attribute::Final;
                            switch (memberMod) {
                            case Attribute::NoModifier:
                            case Attribute::Static: 
                                {
                                    // Set type to FUTURE_TYPE - it will be resolved during 'Setup'. The value is either FUTURE_VALUE
                                    // for 'const' - in which case the expression is compile time evaluated (or attempted) or set
                                    // to INACCESSIBLE until run time initialization occurs.
                                    Variable *v = new Variable(FUTURE_TYPE, immutable ? JS2VAL_FUTUREVALUE : JS2VAL_INACCESSIBLE, immutable);
                                    vb->member = v;
                                    v->vb = vb;
                                    vb->mn = defineLocalMember(env, name, a->namespaces, a->overrideMod, a->xplicit, ReadWriteAccess, v, p->pos);
                                    bCon->saveMultiname(vb->mn);
                                }
                                break;
                            case Attribute::Virtual:
                            case Attribute::Final: 
                                {
                                    JS2Class *c = checked_cast<JS2Class *>(env->getTopFrame());
                                    InstanceMember *m = new InstanceVariable(FUTURE_TYPE, immutable, (memberMod == Attribute::Final), c->slotCount++);
                                    vb->member = m;
                                    vb->osp = defineInstanceMember(c, cxt, name, a->namespaces, a->overrideMod, a->xplicit, ReadWriteAccess, m, p->pos);
                                }
                                break;
                            default:
                                reportError(Exception::definitionError, "Illegal attribute", p->pos);
                                break;
                            }
                        }
                        vb = vb->next;
                    }
                }
                break;
            case StmtNode::expression:
                {
                    ExprStmtNode *e = checked_cast<ExprStmtNode *>(p);
                    ValidateExpression(cxt, env, e->expr);
                }
                break;
            case StmtNode::Namespace:
                {
                    NamespaceStmtNode *ns = checked_cast<NamespaceStmtNode *>(p);
                    Attribute *attr = NULL;
                    if (ns->attributes) {
                        ValidateAttributeExpression(cxt, env, ns->attributes);
                        attr = EvalAttributeExpression(env, CompilePhase, ns->attributes);
                    }
                    a = Attribute::toCompoundAttribute(attr);
                    if (a->dynamic || a->prototype)
                        reportError(Exception::definitionError, "Illegal attribute", p->pos);
                    if ( ! ((a->memberMod == Attribute::NoModifier) || ((a->memberMod == Attribute::Static) && (env->getTopFrame()->kind == ClassKind))) )
                        reportError(Exception::definitionError, "Illegal attribute", p->pos);
                    Variable *v = new Variable(namespaceClass, OBJECT_TO_JS2VAL(new Namespace(&ns->name)), true);
                    defineLocalMember(env, &ns->name, a->namespaces, a->overrideMod, a->xplicit, ReadWriteAccess, v, p->pos);
                }
                break;
            case StmtNode::Use:
                {
                    UseStmtNode *u = checked_cast<UseStmtNode *>(p);
                    ExprList *eList = u->namespaces;
                    while (eList) {
                        js2val av = EvalExpression(env, CompilePhase, eList->expr);
                        if (JS2VAL_IS_NULL(av) || !JS2VAL_IS_OBJECT(av))
                            reportError(Exception::badValueError, "Namespace expected in use directive", p->pos);
                        JS2Object *obj = JS2VAL_TO_OBJECT(av);
                        if ((obj->kind != AttributeObjectKind) || (checked_cast<Attribute *>(obj)->attrKind != Attribute::NamespaceAttr))
                            reportError(Exception::badValueError, "Namespace expected in use directive", p->pos);
                        cxt->openNamespaces.push_back(checked_cast<Namespace *>(obj));                    
                        eList = eList->next;
                    }
                }
                break;
            case StmtNode::Class:
                {
                    ClassStmtNode *classStmt = checked_cast<ClassStmtNode *>(p);
                    JS2Class *superClass = objectClass;
                    if (classStmt->superclass) {
                        ValidateExpression(cxt, env, classStmt->superclass);                    
                        js2val av = EvalExpression(env, CompilePhase, classStmt->superclass);
                        if (JS2VAL_IS_NULL(av) || !JS2VAL_IS_OBJECT(av))
                            reportError(Exception::badValueError, "Class expected in inheritance", p->pos);
                        JS2Object *obj = JS2VAL_TO_OBJECT(av);
                        if (obj->kind != ClassKind)
                            reportError(Exception::badValueError, "Class expected in inheritance", p->pos);
                        superClass = checked_cast<JS2Class *>(obj);
                    }
                    Attribute *attr = NULL;
                    if (classStmt->attributes) {
                        ValidateAttributeExpression(cxt, env, classStmt->attributes);
                        attr = EvalAttributeExpression(env, CompilePhase, classStmt->attributes);
                    }
                    a = Attribute::toCompoundAttribute(attr);
                    if (!superClass->complete || superClass->final)
                        reportError(Exception::definitionError, "Illegal inheritance", p->pos);
                    JS2Object *proto = NULL;
                    bool final = false;
                    switch (a->memberMod) {
                    case Attribute::NoModifier: 
                        final = false; 
                        break;
                    case Attribute::Static: 
                        if (env->getTopFrame()->kind != ClassKind)
                            reportError(Exception::definitionError, "Illegal use of static modifier", p->pos);
                        final = false;
                        break;
                    case Attribute::Final:
                        final = true;
                        break;
                    default:
                        reportError(Exception::definitionError, "Illegal modifier for class definition", p->pos);
                        break;
                    }
                    JS2Class *c = new JS2Class(superClass, proto, new Namespace(engine->private_StringAtom), (a->dynamic || superClass->dynamic), true, final, engine->allocStringPtr(&classStmt->name));
                    classStmt->c = c;
                    Variable *v = new Variable(classClass, OBJECT_TO_JS2VAL(c), true);
                    defineLocalMember(env, &classStmt->name, a->namespaces, a->overrideMod, a->xplicit, ReadWriteAccess, v, p->pos);
                    if (classStmt->body) {
                        env->addFrame(c);
                        ValidateStmtList(cxt, env, pl, classStmt->body->statements);
                        ASSERT(env->getTopFrame() == c);
                        env->removeTopFrame();
                    }
                    c->complete = true;
                }
                break;
            case StmtNode::With:
                {
                    UnaryStmtNode *w = checked_cast<UnaryStmtNode *>(p);
                    ValidateExpression(cxt, env, w->expr);
                    ValidateStmt(cxt, env, pl, w->stmt);
                }
                break;
            case StmtNode::empty:
                break;
            default:
                NOT_REACHED("Not Yet Implemented");
            }   // switch (p->getKind())
        }
        catch (Exception x) {
            env->setTopFrame(curTopFrame);
            throw x;
        }
    }


    JS2Class *JS2Metadata::getVariableType(Variable *v, Phase phase, size_t pos)
    {
        JS2Class *type = v->type;
        if (type == NULL) { // Inaccessible, Note that this can only happen when phase = compile 
                            // because the compilation phase ensures that all types are valid, 
                            // so invalid types will not occur during the run phase.          
            ASSERT(phase == CompilePhase);
            reportError(Exception::compileExpressionError, "No type assigned", pos);
        }
        else {
            if (v->type == FUTURE_TYPE) {
                // Note that phase = compile because all futures are resolved by the end of the compilation phase.
                ASSERT(phase == CompilePhase);
                if (v->vb->type) {
                    v->type = NULL;
                    v->type = EvalTypeExpression(env, CompilePhase, v->vb->type);
                }
                else
                    v->type = objectClass;
            }
        }
        return v->type;
    }

    /*
     * Process an individual statement 'p', including it's children
     *  - this generates bytecode for each statement, but doesn't actually
     * execute it.
     */
    void JS2Metadata::SetupStmt(Environment *env, Phase phase, StmtNode *p) 
    {
        JS2Class *exprType;
        switch (p->getKind()) {
        case StmtNode::block:
        case StmtNode::group:
            {
                BlockStmtNode *b = checked_cast<BlockStmtNode *>(p);
                BlockFrame *runtimeFrame = new BlockFrame(b->compileFrame);
                env->addFrame(runtimeFrame);    // XXX is this right? shouldn't this be the compile frame until execution occurs?
                bCon->emitOp(ePushFrame, p->pos);
                bCon->addFrame(runtimeFrame);
                StmtNode *bp = b->statements;
                while (bp) {
                    SetupStmt(env, phase, bp);
                    bp = bp->next;
                }
                bCon->emitOp(ePopFrame, p->pos);
                env->removeTopFrame();
            }
            break;
        case StmtNode::label:
            {
                LabelStmtNode *l = checked_cast<LabelStmtNode *>(p);
                SetupStmt(env, phase, l->stmt);
            }
            break;
        case StmtNode::If:
            {
                BytecodeContainer::LabelID skipOverStmt = bCon->getLabel();
                UnaryStmtNode *i = checked_cast<UnaryStmtNode *>(p);
                Reference *r = SetupExprNode(env, phase, i->expr, &exprType);
                if (r) r->emitReadBytecode(bCon, p->pos);
                bCon->emitBranch(eBranchFalse, skipOverStmt, p->pos);
                SetupStmt(env, phase, i->stmt);
                bCon->setLabel(skipOverStmt);
            }
            break;
        case StmtNode::IfElse:
            {
                BytecodeContainer::LabelID falseStmt = bCon->getLabel();
                BytecodeContainer::LabelID skipOverFalseStmt = bCon->getLabel();
                BinaryStmtNode *i = checked_cast<BinaryStmtNode *>(p);
                Reference *r = SetupExprNode(env, phase, i->expr, &exprType);
                if (r) r->emitReadBytecode(bCon, p->pos);
                bCon->emitBranch(eBranchFalse, falseStmt, p->pos);
                SetupStmt(env, phase, i->stmt);
                bCon->emitBranch(eBranch, skipOverFalseStmt, p->pos);
                bCon->setLabel(falseStmt);
                SetupStmt(env, phase, i->stmt2);
                bCon->setLabel(skipOverFalseStmt);
            }
            break;
        case StmtNode::Break:
            // XXX for break - if there's a finally that applies to this block, it should
            // be invoked at this point - need to track the appropriate label and emit
            // eCallFinally here.
        case StmtNode::Continue:
            {
                GoStmtNode *g = checked_cast<GoStmtNode *>(p);
                bCon->emitBranch(eBranch, g->tgtID, p->pos);
            }
            break;
        case StmtNode::ForIn:
/*
            iterator = get_first_property_of(object) [eFirst]
            //
            // pushes iterator object on stack, returns true/false
            //
            if (false) --> break;
            top:
                v <-- iterator.value  // v is the thing specified by for ('v' in object) ...
                <statement body>
            continue:
                //
                // expect iterator object on top of stack
                // increment it. Returns true/false
                //
                iterator = get_next_property_of(object, iterator) [eNext]
                if (true) --> top;
            break:
                // want stack cleared of iterator at this point
*/
            {
                ForStmtNode *f = checked_cast<ForStmtNode *>(p);
                BytecodeContainer::LabelID loopTop = bCon->getLabel();

                Reference *r = SetupExprNode(env, phase, f->expr2, &exprType);
                if (r) r->emitReadBytecode(bCon, p->pos);
                bCon->emitOp(eFirst, p->pos);
                bCon->emitBranch(eBranchFalse, f->breakLabelID, p->pos);

                bCon->setLabel(loopTop);                
                
                targetList.push_back(p);
                bCon->emitOp(eForValue, p->pos);

                Reference *v = NULL;
                if (f->initializer->getKind() == StmtNode::Var) {
                    VariableStmtNode *vs = checked_cast<VariableStmtNode *>(f->initializer);
                    VariableBinding *vb = vs->bindings;
                    v = new LexicalReference(vb->name, cxt.strict);
                }
                else {
                    if (f->initializer->getKind() == StmtNode::expression) {
                        ExprStmtNode *e = checked_cast<ExprStmtNode *>(f->initializer);
                        v = SetupExprNode(env, phase, e->expr, &exprType);
                        if (v == NULL)
                            reportError(Exception::semanticError, "for..in needs an lValue", p->pos);
                    }
                    else
                        NOT_REACHED("what else??");
                }            
                switch (v->hasStackEffect()) {
                case 1:
                    bCon->emitOp(eSwap, p->pos);
                    break;
                case 2:
                    bCon->emitOp(eSwap2, p->pos);
                    break;
                }
                v->emitWriteBytecode(bCon, p->pos);
                bCon->emitOp(ePop, p->pos);     // clear iterator value from stack
                SetupStmt(env, phase, f->stmt);
                targetList.pop_back();
                bCon->setLabel(f->continueLabelID);
                bCon->emitOp(eNext, p->pos);
                bCon->emitBranch(eBranchTrue, loopTop, p->pos);
                bCon->setLabel(f->breakLabelID);
                bCon->emitOp(ePop, p->pos);
            }
            break;
        case StmtNode::For:
            {
                ForStmtNode *f = checked_cast<ForStmtNode *>(p);
                BytecodeContainer::LabelID loopTop = bCon->getLabel();
                BytecodeContainer::LabelID testLocation = bCon->getLabel();

                if (f->initializer)
                    SetupStmt(env, phase, f->initializer);
                if (f->expr2)
                    bCon->emitBranch(eBranch, testLocation, p->pos);
                bCon->setLabel(loopTop);
                targetList.push_back(p);
                SetupStmt(env, phase, f->stmt);
                targetList.pop_back();
                bCon->setLabel(f->continueLabelID);
                if (f->expr3) {
                    Reference *r = SetupExprNode(env, phase, f->expr3, &exprType);
                    if (r) r->emitReadBytecode(bCon, p->pos);
                    bCon->emitOp(ePop, p->pos);
                }
                bCon->setLabel(testLocation);
                if (f->expr2) {
                    Reference *r = SetupExprNode(env, phase, f->expr2, &exprType);
                    if (r) r->emitReadBytecode(bCon, p->pos);
                    bCon->emitBranch(eBranchTrue, loopTop, p->pos);
                }
                else
                    bCon->emitBranch(eBranch, loopTop, p->pos);
                bCon->setLabel(f->breakLabelID);
            }
            break;
        case StmtNode::Switch:
/*
            <swexpr>        
            eSlotWrite    <switchTemp>

        // test sequence in source order except 
        // the default is moved to end.

            eSlotRead    <switchTemp>
            <case1expr>
            Equal
            BranchTrue --> case1StmtLabel
            eLexicalRead    <switchTemp>
            <case2expr>
            Equal
            BranchTrue --> case2StmtLabel
            Branch --> default, if there is one, or break label

    case1StmtLabel:
            <stmt>
    case2StmtLabel:
            <stmt>
    defaultLabel:
            <stmt>
    case3StmtLabel:
            <stmt>
            ..etc..     // all in source order
    
    breakLabel:
*/
            {
                SwitchStmtNode *sw = checked_cast<SwitchStmtNode *>(p);
                uint16 swVarIndex = (checked_cast<NonWithFrame *>(env->getTopFrame()))->allocateTemp();
                BytecodeContainer::LabelID defaultLabel = NotALabel;

                Reference *r = SetupExprNode(env, phase, sw->expr, &exprType);
                if (r) r->emitReadBytecode(bCon, p->pos);
                bCon->emitOp(eFrameSlotWrite, p->pos);
                bCon->addShort(swVarIndex);

                // First time through, generate the conditional waterfall 
                StmtNode *s = sw->statements;
                while (s) {
                    if (s->getKind() == StmtNode::Case) {
                        ExprStmtNode *c = checked_cast<ExprStmtNode *>(s);
                        if (c->expr) {
                            bCon->emitOp(eFrameSlotRead, c->pos);
                            bCon->addShort(swVarIndex);
                            Reference *r = SetupExprNode(env, phase, c->expr, &exprType);
                            if (r) r->emitReadBytecode(bCon, c->pos);
                            bCon->emitOp(eEqual, c->pos);
                            bCon->emitBranch(eBranchTrue, c->labelID, c->pos);
                        }
                        else
                            defaultLabel = c->labelID;
                    }
                    s = s->next;
                }
                if (defaultLabel == NotALabel)
                    bCon->emitBranch(eBranch, sw->breakLabelID, p->pos);
                else
                    bCon->emitBranch(eBranch, defaultLabel, p->pos);
                // Now emit the contents
                targetList.push_back(p);
                s = sw->statements;
                while (s) {
                    if (s->getKind() == StmtNode::Case) {
                        ExprStmtNode *c = checked_cast<ExprStmtNode *>(s);
                        bCon->setLabel(c->labelID);
                    }
                    else
                        SetupStmt(env, phase, s);
                    s = s->next;
                }
                targetList.pop_back();

                bCon->setLabel(sw->breakLabelID);
            }
            break;
        case StmtNode::While:
            {
                UnaryStmtNode *w = checked_cast<UnaryStmtNode *>(p);
                BytecodeContainer::LabelID loopTop = bCon->getLabel();
                bCon->emitBranch(eBranch, w->continueLabelID, p->pos);
                bCon->setLabel(loopTop);
                targetList.push_back(p);
                SetupStmt(env, phase, w->stmt);
                targetList.pop_back();
                bCon->setLabel(w->continueLabelID);
                Reference *r = SetupExprNode(env, phase, w->expr, &exprType);
                if (r) r->emitReadBytecode(bCon, p->pos);
                bCon->emitBranch(eBranchTrue, loopTop, p->pos);
                bCon->setLabel(w->breakLabelID);
            }
            break;
        case StmtNode::DoWhile:
            {
                UnaryStmtNode *w = checked_cast<UnaryStmtNode *>(p);
                BytecodeContainer::LabelID loopTop = bCon->getLabel();
                bCon->setLabel(loopTop);
                targetList.push_back(p);
                SetupStmt(env, phase, w->stmt);
                targetList.pop_back();
                bCon->setLabel(w->continueLabelID);
                Reference *r = SetupExprNode(env, phase, w->expr, &exprType);
                if (r) r->emitReadBytecode(bCon, p->pos);
                bCon->emitBranch(eBranchTrue, loopTop, p->pos);
                bCon->setLabel(w->breakLabelID);
            }
            break;
        case StmtNode::Throw:
            {
                ExprStmtNode *e = checked_cast<ExprStmtNode *>(p);
                Reference *r = SetupExprNode(env, phase, e->expr, &exprType);
                if (r) r->emitReadBytecode(bCon, p->pos);
                bCon->emitOp(eThrow, p->pos);
            }
            break;
        case StmtNode::Try:
// Your logic is insane and happenstance, like that of a troll
/*
            try {   //  [catchLabel,finallyInvoker] handler labels are pushed on handler stack [eTry]
                    <tryblock>
                }   //  catch handler label is popped off handler stack [eHandler]
                jsr finally
                jump-->finished                 

            finally:        // finally handler label popped off here.
                {           // A throw from in here goes to the next handler on the
                            // handler stack
                }
                rts

            finallyInvoker:         // invoked when an exception is caught in the try block
                push exception      // it arranges to call the finally block and then re-throw
                jsr finally         // the exception - reaching the catch block
                throw exception
            catchLabel:

                    the incoming exception is on the top of the stack at this point

                catch (exception) { // catch handler label popped off   [eHandler]
                        // any throw from in here must jump to the next handler
                        // (i.e. not the this same catch handler!)

                    Of the many catch clauses specified, only the one whose exception variable type
                    matches the type of the incoming exception is executed...

                    dup
                    push type of exception-variable
                    is
                    jumpfalse-->next catch
                    
                    setlocalvar exception-variable
                    pop
                    <catch body>


                }
                // 'normal' fall thru from catch
                jsr finally
                jump finished

            finished:
*/
            {
                TryStmtNode *t = checked_cast<TryStmtNode *>(p);
                BytecodeContainer::LabelID catchClauseLabel;
                BytecodeContainer::LabelID finallyInvokerLabel;
                BytecodeContainer::LabelID t_finallyLabel;
                bCon->emitOp(eTry, p->pos);
                if (t->finally) {
                    finallyInvokerLabel = bCon->getLabel();
                    bCon->addFixup(finallyInvokerLabel);            
                    t_finallyLabel = bCon->getLabel(); 
                }
                else {
                    finallyInvokerLabel = NotALabel;
                    bCon->addOffset(NotALabel);
                    t_finallyLabel = NotALabel;
                }
                if (t->catches) {
                    catchClauseLabel = bCon->getLabel();
                    bCon->addFixup(catchClauseLabel);            
                }
                else {
                    catchClauseLabel = NotALabel;
                    bCon->addOffset(NotALabel);
                }
                BytecodeContainer::LabelID finishedLabel = bCon->getLabel();
                SetupStmt(env, phase, t->stmt);

                if (t->finally) {
                    bCon->emitBranch(eCallFinally, t_finallyLabel, p->pos);
                    bCon->emitBranch(eBranch, finishedLabel, p->pos);

                    bCon->setLabel(t_finallyLabel);
                    bCon->emitOp(eHandler, p->pos);
                    SetupStmt(env, phase, t->finally);
                    bCon->emitOp(eReturnFinally, p->pos);

                    bCon->setLabel(finallyInvokerLabel);
                    // the exception object is on the top of the stack already
                    bCon->emitBranch(eCallFinally, t_finallyLabel, p->pos);
                    ASSERT(bCon->mStackTop == 0);
                    bCon->mStackTop = 1;
                    bCon->emitOp(eThrow, p->pos);
                }
                else {
                    bCon->emitBranch(eBranch, finishedLabel, p->pos);
                }

                if (t->catches) {
                    bCon->setLabel(catchClauseLabel);
                    bCon->emitOp(eHandler, p->pos);
                    CatchClause *c = t->catches;
                    // the exception object will be the only thing on the stack
                    ASSERT(bCon->mStackTop == 0);
                    bCon->mStackTop = 1;
                    if (bCon->mStackMax < 1) bCon->mStackMax = 1;
                    BytecodeContainer::LabelID nextCatch = NotALabel;
                    while (c) {                    
                        if (c->next && c->type) {
                            nextCatch = bCon->getLabel();
                            bCon->emitOp(eDup, p->pos);
                            Reference *r = SetupExprNode(env, phase, c->type, &exprType);
                            if (r) r->emitReadBytecode(bCon, p->pos);
                            bCon->emitOp(eIs, p->pos);
                            bCon->emitBranch(eBranchFalse, nextCatch, p->pos);
                        }
                        // write the exception object (on stack top) into the named
                        // local variable
                        Reference *r = new LexicalReference(&c->name, false);
                        r->emitWriteBytecode(bCon, p->pos);
                        bCon->emitOp(ePop, p->pos);
                        SetupStmt(env, phase, c->stmt);
                        if (t->finally) {
                            bCon->emitBranch(eCallFinally, t_finallyLabel, p->pos);
                        }
                        c = c->next;
                        if (c) {
                            bCon->emitBranch(eBranch, finishedLabel, p->pos);
                            bCon->mStackTop = 1;
                            if (nextCatch != NotALabel)
                                bCon->setLabel(nextCatch);
                        }
                    }
                }
                bCon->setLabel(finishedLabel);
            }
            break;
        case StmtNode::Return:
            {
                ExprStmtNode *e = checked_cast<ExprStmtNode *>(p);
                if (e->expr) {
                    Reference *r = SetupExprNode(env, phase, e->expr, &exprType);
                    if (r) r->emitReadBytecode(bCon, p->pos);
                    bCon->emitOp(eReturn, p->pos);
                }
            }
            break;
        case StmtNode::Function:
            {
                FunctionStmtNode *f = checked_cast<FunctionStmtNode *>(p);
                CompilationData *oldData = startCompilationUnit(f->function.fWrap->bCon, bCon->mSource, bCon->mSourceLocation);
                env->addFrame(f->function.fWrap->compileFrame);
#ifdef DEBUG
                bCon->fName = *f->function.name;
#endif
                SetupStmt(env, phase, f->function.body);
                // XXX need to make sure that all paths lead to an exit of some kind
                bCon->emitOp(eReturnVoid, p->pos);
                env->removeTopFrame();
                restoreCompilationUnit(oldData);
            }
            break;
        case StmtNode::Var:
        case StmtNode::Const:
            {
                // Note that the code here is the Setup code plus the emit of the Eval bytecode
                VariableStmtNode *vs = checked_cast<VariableStmtNode *>(p);                
                VariableBinding *vb = vs->bindings;
                while (vb)  {
                    if (vb->member) {   // static or instance variable
                        if (vb->member->kind == Member::Variable) {
                            Variable *v = checked_cast<Variable *>(vb->member);
                            JS2Class *type = getVariableType(v, CompilePhase, p->pos);
                            if (JS2VAL_IS_FUTURE(v->value)) {   // it's a const, execute the initializer
                                v->value = JS2VAL_INACCESSIBLE;
                                if (vb->initializer) {
                                    try {
                                        js2val newValue = EvalExpression(env, CompilePhase, vb->initializer);
                                        v->value = type->implicitCoerce(this, newValue);
                                    }
                                    catch (Exception x) {
                                        // If a compileExpressionError occurred, then the initialiser is 
                                        // not a compile-time constant expression. In this case, ignore the
                                        // error and leave the value of the variable INACCESSIBLE until it
                                        // is defined at run time.
                                        if (x.kind != Exception::compileExpressionError)
                                            throw x;
                                        Reference *r = SetupExprNode(env, phase, vb->initializer, &exprType);
                                        if (r) r->emitReadBytecode(bCon, p->pos);
                                        LexicalReference *lVal = new LexicalReference(vb->mn, cxt.strict);
                                        lVal->emitWriteBytecode(bCon, p->pos);      
                                        bCon->emitOp(ePop, p->pos);
                                    }
                                }
                                else
                                    // Would only have come here if the variable was immutable - i.e. a 'const' definition
                                    // XXX why isn't this handled at validation-time?
                                    reportError(Exception::compileExpressionError, "Missing compile time expression", p->pos);
                            }
                            else {
                                // Not immutable
                                ASSERT(JS2VAL_IS_INACCESSIBLE(v->value));
                                if (vb->initializer) {
                                    Reference *r = SetupExprNode(env, phase, vb->initializer, &exprType);
                                    if (r) r->emitReadBytecode(bCon, p->pos);
                                    bCon->emitOp(eCoerce, p->pos);
                                    bCon->addType(v->type);
                                    LexicalReference *lVal = new LexicalReference(vb->mn, cxt.strict);
                                    lVal->emitInitBytecode(bCon, p->pos);      
                                }
                                else {
                                    v->type->emitDefaultValue(bCon, p->pos);
                                    LexicalReference *lVal = new LexicalReference(vb->mn, cxt.strict);
                                    lVal->emitInitBytecode(bCon, p->pos);      
                                }
                            }
                        }
                        else {
                            ASSERT(vb->member->kind == Member::InstanceVariableKind);
                            InstanceVariable *v = checked_cast<InstanceVariable *>(vb->member);
                            JS2Class *t;
                            if (vb->type)
                                t = EvalTypeExpression(env, CompilePhase, vb->type);
                            else {
                                if (vb->osp->first->overriddenMember && (vb->osp->first->overriddenMember != POTENTIAL_CONFLICT))
                                    t = vb->osp->first->overriddenMember->type;
                                else
                                    if (vb->osp->second->overriddenMember && (vb->osp->second->overriddenMember != POTENTIAL_CONFLICT))
                                        t = vb->osp->second->overriddenMember->type;
                                    else
                                        t = objectClass;
                            }
                            v->type = t;
                        }
                    }
                    else { // HoistedVariable
                        if (vb->initializer) {
                            Reference *r = SetupExprNode(env, phase, vb->initializer, &exprType);
                            if (r) r->emitReadBytecode(bCon, p->pos);
                            LexicalReference *lVal = new LexicalReference(vb->name, cxt.strict);
                            lVal->variableMultiname.addNamespace(publicNamespace);
                            lVal->emitInitBytecode(bCon, p->pos);                                                        
                        }
                    }
                    vb = vb->next;
                }
            }
            break;
        case StmtNode::expression:
            {
                ExprStmtNode *e = checked_cast<ExprStmtNode *>(p);
                Reference *r = SetupExprNode(env, phase, e->expr, &exprType);
                if (r) r->emitReadBytecode(bCon, p->pos);
                bCon->emitOp(ePopv, p->pos);
            }
            break;
        case StmtNode::Namespace:
            {
            }
            break;
        case StmtNode::Use:
            {
            }
            break;
        case StmtNode::Class:
            {
                ClassStmtNode *classStmt = checked_cast<ClassStmtNode *>(p);
                JS2Class *c = classStmt->c;
                if (classStmt->body) {
                    env->addFrame(c);
                    bCon->emitOp(ePushFrame, p->pos);
                    bCon->addFrame(c);
                    StmtNode *bp = classStmt->body->statements;
                    while (bp) {
                        SetupStmt(env, phase, bp);
                        bp = bp->next;
                    }
                    ASSERT(env->getTopFrame() == c);
                    env->removeTopFrame();
                    bCon->emitOp(ePopFrame, p->pos);
                }
            }
            break;
        case StmtNode::With:
            {
                UnaryStmtNode *w = checked_cast<UnaryStmtNode *>(p);
                Reference *r = SetupExprNode(env, phase, w->expr, &exprType);
                if (r) r->emitReadBytecode(bCon, p->pos);
                bCon->emitOp(eWithin, p->pos);
                SetupStmt(env, phase, w->stmt);                        
                bCon->emitOp(eWithout, p->pos);
            }
            break;
        case StmtNode::empty:
            break;
        default:
            NOT_REACHED("Not Yet Implemented");
        }   // switch (p->getKind())
    }


/************************************************************************************
 *
 *  Attributes
 *
 ************************************************************************************/

    //
    // Validate the Attribute expression at p
    // An attribute expression can only be a list of 'juxtaposed' attribute elements
    //
    // Note : "AttributeExpression" here is a different beast than in the spec. - here it
    // describes the entire attribute part of a directive, not just the qualified identifier
    // and other references encountered in an attribute.
    //
    void JS2Metadata::ValidateAttributeExpression(Context *cxt, Environment *env, ExprNode *p)
    {
        switch (p->getKind()) {
        case ExprNode::boolean:
            break;
        case ExprNode::juxtapose:
            {
                BinaryExprNode *j = checked_cast<BinaryExprNode *>(p);
                ValidateAttributeExpression(cxt, env, j->op1);
                ValidateAttributeExpression(cxt, env, j->op2);
            }
            break;
        case ExprNode::identifier:
            {
                const StringAtom &name = checked_cast<IdentifierExprNode *>(p)->name;
                switch (name.tokenKind) {
                case Token::Public:
                    return;
                case Token::Abstract:
                    return;
                case Token::Final:
                    return;
                case Token::Private:
                    {
                        JS2Class *c = env->getEnclosingClass();
                        if (!c)
                            reportError(Exception::syntaxError, "Private can only be used inside a class definition", p->pos);
                    }
                    return;
                case Token::Static:
                    return;
                }
                // fall thru to handle as generic expression element...
            }            
        default:
            {
                ValidateExpression(cxt, env, p);
            }
            break;

        } // switch (p->getKind())
    }

    // Evaluate the Attribute expression rooted at p.
    // An attribute expression can only be a list of 'juxtaposed' attribute elements
    Attribute *JS2Metadata::EvalAttributeExpression(Environment *env, Phase phase, ExprNode *p)
    {
        switch (p->getKind()) {
        case ExprNode::boolean:
            if (checked_cast<BooleanExprNode *>(p)->value)
                return new TrueAttribute();
            else
                return new FalseAttribute();
        case ExprNode::juxtapose:
            {
                BinaryExprNode *j = checked_cast<BinaryExprNode *>(p);
                Attribute *a = EvalAttributeExpression(env, phase, j->op1);
                if (a && (a->attrKind == Attribute::FalseAttr))
                    return a;
                Attribute *b = EvalAttributeExpression(env, phase, j->op2);
                try {
                    return Attribute::combineAttributes(a, b);
                }
                catch (char *err) {
                    reportError(Exception::badValueError, err, p->pos);
                }
            }
            break;

        case ExprNode::identifier:
            {
                const StringAtom &name = checked_cast<IdentifierExprNode *>(p)->name;
                CompoundAttribute *ca = NULL;
                switch (name.tokenKind) {
                case Token::Public:
                    return publicNamespace;
                case Token::Abstract:
                    ca = new CompoundAttribute();
                    ca->memberMod = Attribute::Abstract;
                    return ca;
                case Token::Final:
                    ca = new CompoundAttribute();
                    ca->memberMod = Attribute::Final;
                    return ca;
                case Token::Private:
                    {
                        JS2Class *c = env->getEnclosingClass();
                        return c->privateNamespace;
                    }
                case Token::Static:
                    ca = new CompoundAttribute();
                    ca->memberMod = Attribute::Static;
                    return ca;
                case Token::identifier:
                    if (name == world.identifiers["constructor"]) {
                        ca = new CompoundAttribute();
                        ca->memberMod = Attribute::Constructor;
                        return ca;
                    }
                    else
                    if (name == world.identifiers["override"]) {
                        ca = new CompoundAttribute();
                        ca->overrideMod = Attribute::DoOverride;
                        return ca;
                    }
                    else
                    if (name == world.identifiers["virtual"]) {
                        ca = new CompoundAttribute();
                        ca->memberMod = Attribute::Virtual;
                        return ca;
                    }
                    else
                    if (name == world.identifiers["dynamic"]) {
                        ca = new CompoundAttribute();
                        ca->dynamic = true;
                        return ca;
                    }
                }
            }            
            // fall thru to execute a readReference on the identifier...
        default:
            {
                // anything else (just references of one kind or another) must
                // be compile-time constant values that resolve to namespaces
                js2val av = EvalExpression(env, CompilePhase, p);
                if (JS2VAL_IS_NULL(av) || !JS2VAL_IS_OBJECT(av))
                    reportError(Exception::badValueError, "Namespace expected in attribute", p->pos);
                JS2Object *obj = JS2VAL_TO_OBJECT(av);
                if ((obj->kind != AttributeObjectKind) || (checked_cast<Attribute *>(obj)->attrKind != Attribute::NamespaceAttr))
                    reportError(Exception::badValueError, "Namespace expected in attribute", p->pos);
                return checked_cast<Attribute *>(obj);
            }
            break;

        } // switch (p->getKind())
        return NULL;
    }

    // Combine attributes a & b, reporting errors for incompatibilities
    // a is not false
    Attribute *Attribute::combineAttributes(Attribute *a, Attribute *b)
    {
        if (b && (b->attrKind == FalseAttr)) {
            if (a) delete a;
            return b;
        }
        if (!a || (a->attrKind == TrueAttr)) {
            if (a) delete a;
            return b;
        }
        if (!b || (b->attrKind == TrueAttr)) {
            if (b) delete b;
            return a;
        }
        if (a->attrKind == NamespaceAttr) {
            if (a == b) {
                delete b;
                return a;
            }
            Namespace *na = checked_cast<Namespace *>(a);
            if (b->attrKind == NamespaceAttr) {
                Namespace *nb = checked_cast<Namespace *>(b);
                CompoundAttribute *c = new CompoundAttribute();
                c->addNamespace(na);
                c->addNamespace(nb);
                delete a;
                delete b;
                return (Attribute *)c;
            }
            else {
                ASSERT(b->attrKind == CompoundAttr);
                CompoundAttribute *cb = checked_cast<CompoundAttribute *>(b);
                cb->addNamespace(na);
                delete a;
                return b;
            }
        }
        else {
            // Both a and b are compound attributes. Ensure that they have no conflicting contents.
            ASSERT((a->attrKind == CompoundAttr) && (b->attrKind == CompoundAttr));
            CompoundAttribute *ca = checked_cast<CompoundAttribute *>(a);
            CompoundAttribute *cb = checked_cast<CompoundAttribute *>(b);
            if ((ca->memberMod != NoModifier) && (cb->memberMod != NoModifier) && (ca->memberMod != cb->memberMod))
                throw("Illegal combination of member modifier attributes");
            if ((ca->overrideMod != NoOverride) && (cb->overrideMod != NoOverride) && (ca->overrideMod != cb->overrideMod))
                throw("Illegal combination of override attributes");
            for (NamespaceListIterator i = cb->namespaces->begin(), end = cb->namespaces->end(); (i != end); i++)
                ca->addNamespace(*i);
            ca->xplicit |= cb->xplicit;
            ca->dynamic |= cb->dynamic;
            if (ca->memberMod == NoModifier)
                ca->memberMod = cb->memberMod;
            if (ca->overrideMod == NoOverride)
                ca->overrideMod = cb->overrideMod;
            ca->prototype |= cb->prototype;
            ca->unused |= cb->unused;
            delete b;
            return a;
        }
    }

    // add the namespace to our list, but only if it's not there already
    void CompoundAttribute::addNamespace(Namespace *n)
    {
        if (namespaces) {
            for (NamespaceListIterator i = namespaces->begin(), end = namespaces->end(); (i != end); i++)
                if (*i == n)
                    return;
        }
        else
            namespaces = new NamespaceList();
        namespaces->push_back(n);
    }

    CompoundAttribute::CompoundAttribute() : Attribute(CompoundAttr),
            namespaces(NULL), xplicit(false), dynamic(false), memberMod(NoModifier), 
            overrideMod(NoOverride), prototype(false), unused(false) 
    { 
    }

    // Convert an attribute to a compoundAttribute. If the attribute
    // is NULL, return a default compoundAttribute
    CompoundAttribute *Attribute::toCompoundAttribute(Attribute *a)
    { 
        if (a) 
            return a->toCompoundAttribute(); 
        else
            return new CompoundAttribute();
    }

    // Convert a simple namespace to a compoundAttribute with that namespace
    CompoundAttribute *Namespace::toCompoundAttribute()    
    { 
        CompoundAttribute *t = new CompoundAttribute(); 
        t->addNamespace(this); 
        return t; 
    }

    // Convert a 'true' attribute to a default compoundAttribute
    CompoundAttribute *TrueAttribute::toCompoundAttribute()    
    { 
        return new CompoundAttribute(); 
    }

    // gc-mark all contained JS2Objects and visit contained structures to do likewise
    void CompoundAttribute::markChildren()
    {
        if (namespaces) {
            for (NamespaceListIterator i = namespaces->begin(), end = namespaces->end(); (i != end); i++) {
                GCMARKOBJECT(*i)
            }
        }
    }


/************************************************************************************
 *
 *  Expressions
 *
 ************************************************************************************/

    // Validate the entire expression rooted at p
    void JS2Metadata::ValidateExpression(Context *cxt, Environment *env, ExprNode *p)
    {
        switch (p->getKind()) {
        case ExprNode::Null:
        case ExprNode::number:
        case ExprNode::regExp:
        case ExprNode::arrayLiteral:
        case ExprNode::numUnit:
        case ExprNode::string:
        case ExprNode::boolean:
            break;
        case ExprNode::This:
            {
                if (env->findThis(true) == JS2VAL_VOID)
                    reportError(Exception::syntaxError, "No 'this' available", p->pos);
            }
            break;
        case ExprNode::objectLiteral:
            break;
        case ExprNode::index:
            {
                InvokeExprNode *i = checked_cast<InvokeExprNode *>(p);
                ValidateExpression(cxt, env, i->op);
                ExprPairList *ep = i->pairs;
                uint16 positionalCount = 0;
                // XXX errors below should only occur at runtime - insert code to throw exception
                // or let the bytecodes handle (and throw on) multiple & named arguments?
                while (ep) {
                    if (ep->field)
                        reportError(Exception::argumentMismatchError, "Indexing doesn't support named arguments", p->pos);
                    else {
                        if (positionalCount)
                            reportError(Exception::argumentMismatchError, "Indexing doesn't support more than 1 argument", p->pos);
                        positionalCount++;
                        ValidateExpression(cxt, env, ep->value);
                    }
                    ep = ep->next;
                }
            }
            break;
        case ExprNode::dot:
            {
                BinaryExprNode *b = checked_cast<BinaryExprNode *>(p);
                ValidateExpression(cxt, env, b->op1);
                ValidateExpression(cxt, env, b->op2);
            }
            break;

        case ExprNode::lessThan:
        case ExprNode::lessThanOrEqual:
        case ExprNode::greaterThan:
        case ExprNode::greaterThanOrEqual:
        case ExprNode::equal:
        case ExprNode::notEqual:
        case ExprNode::assignment:
        case ExprNode::add:
        case ExprNode::subtract:
        case ExprNode::multiply:
        case ExprNode::divide:
        case ExprNode::modulo:
        case ExprNode::addEquals:
        case ExprNode::subtractEquals:
        case ExprNode::multiplyEquals:
        case ExprNode::divideEquals:
        case ExprNode::moduloEquals:
        case ExprNode::logicalAnd:
        case ExprNode::logicalXor:
        case ExprNode::logicalOr:
        case ExprNode::leftShift:
        case ExprNode::rightShift:
        case ExprNode::logicalRightShift:
        case ExprNode::bitwiseAnd:
        case ExprNode::bitwiseXor:
        case ExprNode::bitwiseOr:
        case ExprNode::leftShiftEquals:
        case ExprNode::rightShiftEquals:
        case ExprNode::logicalRightShiftEquals:
        case ExprNode::bitwiseAndEquals:
        case ExprNode::bitwiseXorEquals:
        case ExprNode::bitwiseOrEquals:
        case ExprNode::logicalAndEquals:
        case ExprNode::logicalXorEquals:
        case ExprNode::logicalOrEquals:
        case ExprNode::comma:
        case ExprNode::Instanceof:
        case ExprNode::identical:
        case ExprNode::notIdentical:

            {
                BinaryExprNode *b = checked_cast<BinaryExprNode *>(p);
                ValidateExpression(cxt, env, b->op1);
                ValidateExpression(cxt, env, b->op2);
            }
            break;

        case ExprNode::Delete:
        case ExprNode::minus:
        case ExprNode::plus:
        case ExprNode::complement:
        case ExprNode::postIncrement:
        case ExprNode::postDecrement:
        case ExprNode::preIncrement:
        case ExprNode::preDecrement:
        case ExprNode::parentheses:
        case ExprNode::Typeof:
        case ExprNode::logicalNot:
        case ExprNode::Void: 
            {
                UnaryExprNode *u = checked_cast<UnaryExprNode *>(p);
                ValidateExpression(cxt, env, u->op);
            }
            break;

        case ExprNode::conditional:
            {
                TernaryExprNode *c = checked_cast<TernaryExprNode *>(p);
                ValidateExpression(cxt, env, c->op1);
                ValidateExpression(cxt, env, c->op2);
                ValidateExpression(cxt, env, c->op3);
            }
            break;

        case ExprNode::qualify:
        case ExprNode::identifier:
            {
//                IdentifierExprNode *i = checked_cast<IdentifierExprNode *>(p);
            }
            break;
        case ExprNode::call:
            {
                InvokeExprNode *i = checked_cast<InvokeExprNode *>(p);
                ValidateExpression(cxt, env, i->op);
                ExprPairList *args = i->pairs;
                while (args) {
                    ValidateExpression(cxt, env, args->value);
                    args = args->next;
                }
            }
            break;
        case ExprNode::New: 
            {
                InvokeExprNode *i = checked_cast<InvokeExprNode *>(p);
                ValidateExpression(cxt, env, i->op);
                ExprPairList *args = i->pairs;
                while (args) {
                    ValidateExpression(cxt, env, args->value);
                    args = args->next;
                }
            }
            break;
        case ExprNode::functionLiteral:
            {
                FunctionExprNode *f = checked_cast<FunctionExprNode *>(p);
                f->obj = validateStaticFunction(&f->function, JS2VAL_INACCESSIBLE, true, true, cxt, env);
            }
            break;
        default:
            NOT_REACHED("Not Yet Implemented");
        } // switch (p->getKind())
    }



    /*
     * Process the expression (i.e. generate bytecode, but don't execute) rooted at p.
     */
    Reference *JS2Metadata::SetupExprNode(Environment *env, Phase phase, ExprNode *p, JS2Class **exprType)
    {
        Reference *returnRef = NULL;
        *exprType = NULL;
        JS2Op op;

        switch (p->getKind()) {

        case ExprNode::parentheses:
            {
                UnaryExprNode *u = checked_cast<UnaryExprNode *>(p);
                returnRef = SetupExprNode(env, phase, u->op, exprType);
            }
            break;
        case ExprNode::assignment:
            {
                if (phase == CompilePhase) reportError(Exception::compileExpressionError, "Inappropriate compile time expression", p->pos);
                BinaryExprNode *b = checked_cast<BinaryExprNode *>(p);
                Reference *lVal = SetupExprNode(env, phase, b->op1, exprType);
                JS2Class *l_exprType = *exprType;
                if (lVal) {
                    Reference *rVal = SetupExprNode(env, phase, b->op2, exprType);
                    if (rVal) rVal->emitReadBytecode(bCon, p->pos);
                    lVal->emitWriteBytecode(bCon, p->pos);
                    *exprType = l_exprType;
                }
                else
                    reportError(Exception::semanticError, "Assignment needs an lValue", p->pos);
            }
            break;
        case ExprNode::leftShiftEquals:
            op = eLeftShift;
            goto doAssignBinary;
        case ExprNode::rightShiftEquals:
            op = eRightShift;
            goto doAssignBinary;
        case ExprNode::logicalRightShiftEquals:
            op = eLogicalRightShift;
            goto doAssignBinary;
        case ExprNode::bitwiseAndEquals:
            op = eBitwiseAnd;
            goto doAssignBinary;
        case ExprNode::bitwiseXorEquals:
            op = eBitwiseXor;
            goto doAssignBinary;
        case ExprNode::logicalXorEquals:
            op = eLogicalXor;
            goto doAssignBinary;
        case ExprNode::bitwiseOrEquals:
            op = eBitwiseOr;
            goto doAssignBinary;
        case ExprNode::addEquals:
            op = eAdd;
            goto doAssignBinary;
        case ExprNode::subtractEquals:
            op = eSubtract;
            goto doAssignBinary;
        case ExprNode::multiplyEquals:
            op = eMultiply;
            goto doAssignBinary;
        case ExprNode::divideEquals:
            op = eDivide;
            goto doAssignBinary;
        case ExprNode::moduloEquals:
            op = eModulo;
            goto doAssignBinary;
doAssignBinary:
            {
                if (phase == CompilePhase) reportError(Exception::compileExpressionError, "Inappropriate compile time expression", p->pos);
                BinaryExprNode *b = checked_cast<BinaryExprNode *>(p);
                Reference *lVal = SetupExprNode(env, phase, b->op1, exprType);
                JS2Class *l_exprType = *exprType;
                if (lVal) {
                    lVal->emitReadForWriteBackBytecode(bCon, p->pos);
                    Reference *rVal = SetupExprNode(env, phase, b->op2, exprType);
                    if (rVal) rVal->emitReadBytecode(bCon, p->pos);
                    *exprType = l_exprType;
                }
                else
                    reportError(Exception::semanticError, "Assignment needs an lValue", p->pos);
                bCon->emitOp(op, p->pos);
                lVal->emitWriteBackBytecode(bCon, p->pos);
            }
            break;
        case ExprNode::lessThan:
            op = eLess;
            goto boolBinary;
        case ExprNode::lessThanOrEqual:
            op = eLessEqual;
            goto boolBinary;
        case ExprNode::greaterThan:
            op = eGreater;
            goto boolBinary;
        case ExprNode::greaterThanOrEqual:
            op = eGreaterEqual;
            goto boolBinary;
        case ExprNode::equal:
            op = eEqual;
            goto boolBinary;
        case ExprNode::notEqual:
            op = eNotEqual;
            goto boolBinary;
        case ExprNode::identical:
            op = eEqual;
            goto boolBinary;
        case ExprNode::notIdentical:
            op = eNotEqual;
            goto boolBinary;
boolBinary:
            *exprType = booleanClass;
            goto doBinary;

        case ExprNode::leftShift:
            op = eLeftShift;
            goto doBinary;
        case ExprNode::rightShift:
            op = eRightShift;
            goto doBinary;
        case ExprNode::logicalRightShift:
            op = eLogicalRightShift;
            goto doBinary;
        case ExprNode::bitwiseAnd:
            op = eBitwiseAnd;
            goto doBinary;
        case ExprNode::bitwiseXor:
            op = eBitwiseXor;
            goto doBinary;
        case ExprNode::bitwiseOr:
            op = eBitwiseOr;
            goto doBinary;

        case ExprNode::add:
            op = eAdd;
            goto doBinary;
        case ExprNode::subtract:
            op = eSubtract;
            goto doBinary;
        case ExprNode::multiply:
            op = eMultiply;
            goto doBinary;
        case ExprNode::divide:
            op = eDivide;
            goto doBinary;
        case ExprNode::modulo:
            op = eModulo;
            goto doBinary;
doBinary:
            {
                JS2Class *l_exprType, *r_exprType;
                BinaryExprNode *b = checked_cast<BinaryExprNode *>(p);
                Reference *lVal = SetupExprNode(env, phase, b->op1, &l_exprType);
                if (lVal) lVal->emitReadBytecode(bCon, p->pos);
                Reference *rVal = SetupExprNode(env, phase, b->op2, &r_exprType);
                if (rVal) rVal->emitReadBytecode(bCon, p->pos);
                bCon->emitOp(op, p->pos);
            }
            break;
        case ExprNode::Void: 
            {
                UnaryExprNode *u = checked_cast<UnaryExprNode *>(p);
                SetupExprNode(env, phase, u->op, exprType);
                bCon->emitOp(eVoid, p->pos);
            }
            break;

        case ExprNode::logicalNot:
            op = eLogicalNot;
            goto doUnary;
        case ExprNode::minus:
            op = eMinus;
            goto doUnary;
        case ExprNode::plus:
            op = ePlus;
            goto doUnary;
        case ExprNode::complement:
            op = eComplement;
            goto doUnary;
doUnary:
            {
                UnaryExprNode *u = checked_cast<UnaryExprNode *>(p);
                Reference *rVal = SetupExprNode(env, phase, u->op, exprType);
                if (rVal) rVal->emitReadBytecode(bCon, p->pos);
                bCon->emitOp(op, p->pos);
            }
            break;

        case ExprNode::logicalAndEquals:
            {
                if (phase == CompilePhase) reportError(Exception::compileExpressionError, "Inappropriate compile time expression", p->pos);
                BinaryExprNode *b = checked_cast<BinaryExprNode *>(p);
                BytecodeContainer::LabelID skipOverSecondHalf = bCon->getLabel();
                Reference *lVal = SetupExprNode(env, phase, b->op1, exprType);
                if (lVal) 
                    lVal->emitReadForWriteBackBytecode(bCon, p->pos);
                else
                    reportError(Exception::semanticError, "Assignment needs an lValue", p->pos);
                bCon->emitOp(eDup, p->pos);
                bCon->emitBranch(eBranchFalse, skipOverSecondHalf, p->pos);
                bCon->emitOp(ePop, p->pos);
                Reference *rVal = SetupExprNode(env, phase, b->op2, exprType);
                if (rVal) rVal->emitReadBytecode(bCon, p->pos);
                bCon->setLabel(skipOverSecondHalf);
                lVal->emitWriteBackBytecode(bCon, p->pos);
            }
            break;

        case ExprNode::logicalOrEquals:
            {
                if (phase == CompilePhase) reportError(Exception::compileExpressionError, "Inappropriate compile time expression", p->pos);
                BinaryExprNode *b = checked_cast<BinaryExprNode *>(p);
                BytecodeContainer::LabelID skipOverSecondHalf = bCon->getLabel();
                Reference *lVal = SetupExprNode(env, phase, b->op1, exprType);
                if (lVal) 
                    lVal->emitReadForWriteBackBytecode(bCon, p->pos);
                else
                    reportError(Exception::semanticError, "Assignment needs an lValue", p->pos);
                bCon->emitOp(eDup, p->pos);
                bCon->emitBranch(eBranchTrue, skipOverSecondHalf, p->pos);
                bCon->emitOp(ePop, p->pos);
                Reference *rVal = SetupExprNode(env, phase, b->op2, exprType);
                if (rVal) rVal->emitReadBytecode(bCon, p->pos);
                bCon->setLabel(skipOverSecondHalf);
                lVal->emitWriteBackBytecode(bCon, p->pos);
            }
            break;

        case ExprNode::logicalAnd:
            {
                BinaryExprNode *b = checked_cast<BinaryExprNode *>(p);
                BytecodeContainer::LabelID skipOverSecondHalf = bCon->getLabel();
                Reference *lVal = SetupExprNode(env, phase, b->op1, exprType);
                if (lVal) lVal->emitReadBytecode(bCon, p->pos);
                bCon->emitOp(eDup, p->pos);
                bCon->emitBranch(eBranchFalse, skipOverSecondHalf, p->pos);
                bCon->emitOp(ePop, p->pos);
                Reference *rVal = SetupExprNode(env, phase, b->op2, exprType);
                if (rVal) rVal->emitReadBytecode(bCon, p->pos);
                bCon->setLabel(skipOverSecondHalf);
            }
            break;

        case ExprNode::logicalXor:
            {
                BinaryExprNode *b = checked_cast<BinaryExprNode *>(p);
                Reference *lVal = SetupExprNode(env, phase, b->op1, exprType);
                if (lVal) lVal->emitReadBytecode(bCon, p->pos);
                Reference *rVal = SetupExprNode(env, phase, b->op2, exprType);
                if (rVal) rVal->emitReadBytecode(bCon, p->pos);
                bCon->emitOp(eLogicalXor, p->pos);
            }
            break;

        case ExprNode::logicalOr:
            {
                BinaryExprNode *b = checked_cast<BinaryExprNode *>(p);
                BytecodeContainer::LabelID skipOverSecondHalf = bCon->getLabel();
                Reference *lVal = SetupExprNode(env, phase, b->op1, exprType);
                if (lVal) lVal->emitReadBytecode(bCon, p->pos);
                bCon->emitOp(eDup, p->pos);
                bCon->emitBranch(eBranchTrue, skipOverSecondHalf, p->pos);
                bCon->emitOp(ePop, p->pos);
                Reference *rVal = SetupExprNode(env, phase, b->op2, exprType);
                if (rVal) rVal->emitReadBytecode(bCon, p->pos);
                bCon->setLabel(skipOverSecondHalf);
            }
            break;

        case ExprNode::This:
            {
                bCon->emitOp(eThis, p->pos);
            }
            break;
        case ExprNode::Null:
            {
                bCon->emitOp(eNull, p->pos);
            }
            break;
        case ExprNode::numUnit:
            {
                NumUnitExprNode *n = checked_cast<NumUnitExprNode *>(p);
                if (n->str.compare(String(widenCString("UL"))) == 0)
                    bCon->addUInt64((uint64)(n->num), p->pos);
                else
                    if (n->str.compare(String(widenCString("L"))) == 0)
                        bCon->addInt64((uint64)(n->num), p->pos);
                    else
                        reportError(Exception::badValueError, "Unrecognized unit", p->pos);
            }
            break;
        case ExprNode::number:
            {
                bCon->addFloat64(checked_cast<NumberExprNode *>(p)->value, p->pos);
            }
            break;
        case ExprNode::regExp:
            {
                RegExpExprNode *v = checked_cast<RegExpExprNode *>(p);
                js2val args[2];
                args[0] = engine->allocString(v->re);
                args[1] = engine->allocString(&v->flags);
                // XXX error handling during this parse? The RegExp_Constructor is
                // going to call errorPos() on the current bCon.
                js2val reValue = RegExp_Constructor(this, JS2VAL_NULL, args, 2);
                RegExpInstance *reInst = checked_cast<RegExpInstance *>(JS2VAL_TO_OBJECT(reValue));
                bCon->addRegExp(reInst, p->pos);
            }
            break;
        case ExprNode::string:
            {  
                bCon->addString(checked_cast<StringExprNode *>(p)->str, p->pos);
            }
            break;
        case ExprNode::conditional:
            {
                BytecodeContainer::LabelID falseConditionExpression = bCon->getLabel();
                BytecodeContainer::LabelID labelAtBottom = bCon->getLabel();

                TernaryExprNode *c = checked_cast<TernaryExprNode *>(p);
                Reference *lVal = SetupExprNode(env, phase, c->op1, exprType);
                if (lVal) lVal->emitReadBytecode(bCon, p->pos);
                bCon->emitBranch(eBranchFalse, falseConditionExpression, p->pos);

                lVal = SetupExprNode(env, phase, c->op2, exprType);
                if (lVal) lVal->emitReadBytecode(bCon, p->pos);
                bCon->emitBranch(eBranch, labelAtBottom, p->pos);

                bCon->setLabel(falseConditionExpression);
                //adjustStack(-1);        // the true case will leave a stack entry pending
                                          // but we can discard it since only one path will be taken.
                lVal = SetupExprNode(env, phase, c->op3, exprType);
                if (lVal) lVal->emitReadBytecode(bCon, p->pos);

                bCon->setLabel(labelAtBottom);
            }
            break;
        case ExprNode::qualify:
            {
                QualifyExprNode *qe = checked_cast<QualifyExprNode *>(p);
                const StringAtom &name = qe->name;

                js2val av = EvalExpression(env, CompilePhase, qe->qualifier);
                if (JS2VAL_IS_NULL(av) || !JS2VAL_IS_OBJECT(av))
                    reportError(Exception::badValueError, "Namespace expected in qualifier", p->pos);
                JS2Object *obj = JS2VAL_TO_OBJECT(av);
                if ((obj->kind != AttributeObjectKind) || (checked_cast<Attribute *>(obj)->attrKind != Attribute::NamespaceAttr))
                    reportError(Exception::badValueError, "Namespace expected in qualifier", p->pos);
                Namespace *ns = checked_cast<Namespace *>(obj);
                
                returnRef = new LexicalReference(&name, ns, cxt.strict);
            }
            break;
        case ExprNode::identifier:
            {
                IdentifierExprNode *i = checked_cast<IdentifierExprNode *>(p);
                returnRef = new LexicalReference(&i->name, cxt.strict);
                ((LexicalReference *)returnRef)->variableMultiname.addNamespace(cxt);
                
                // Try to find this identifier at compile time, we have to stop if we reach
                // a frame that supports dynamic properties - the identifier could be
                // created at runtime without us finding it here.
                Multiname *multiname = &((LexicalReference *)returnRef)->variableMultiname;
                FrameListIterator fi = env->getBegin();
                while (fi != env->getEnd()) {
                    Frame *fr = *fi;
                    if (fr->kind == WithFrameKind)
                        // XXX unless it's provably not a dynamic object that been with'd??
                        break;
                    NonWithFrame *pf = checked_cast<NonWithFrame *>(*fi);
                    if (pf->kind != ClassKind) {
                        LocalMember *m = findFlatMember(pf, multiname, ReadAccess, CompilePhase);
                        if (m && m->kind == Member::Variable) {
                            *exprType = checked_cast<Variable *>(m)->type;
                            break;
                        }
                        if (pf->kind == GlobalObjectKind)
                            break;
                    }
                    else {
                        JS2Class *c = checked_cast<JS2Class *>(pf);
                        MemberDescriptor m2;
                        if (findLocalMember(c, multiname, ReadAccess, CompilePhase, &m2) 
                                && m2.localMember) {
                            if (m2.localMember->kind == LocalMember::Variable)
                                *exprType = checked_cast<Variable *>(m2.localMember)->type;
                            break;
                        }
                        if (m2.ns) {   // an instance member
                            QualifiedName qname(m2.ns, multiname->name);
                            InstanceMember *m = findInstanceMember(c, &qname, ReadAccess);
                            if (m) {
                                if (m->kind == InstanceMember::InstanceVariableKind)
                                    *exprType = checked_cast<InstanceVariable *>(m)->type;
                                break;
                            }
                            else
                                break;  // XXX Shouldn't findLocalMember guarantee this not possible?
                        }
                        else
                            break;
                            // XXX ok to keep going? Suppose the class allows dynamic properties?
                    }
                    fi++;
                }
            }
            break;
        case ExprNode::Delete:
            {
                UnaryExprNode *u = checked_cast<UnaryExprNode *>(p);
                Reference *lVal = SetupExprNode(env, phase, u->op, exprType);
                if (lVal)
                    lVal->emitDeleteBytecode(bCon, p->pos);
                else
                    reportError(Exception::semanticError, "Delete needs an lValue", p->pos);
            }
            break;
        case ExprNode::postIncrement:
            {
                UnaryExprNode *u = checked_cast<UnaryExprNode *>(p);
                Reference *lVal = SetupExprNode(env, phase, u->op, exprType);
                if (lVal)
                    lVal->emitPostIncBytecode(bCon, p->pos);
                else
                    reportError(Exception::semanticError, "PostIncrement needs an lValue", p->pos);
            }
            break;
        case ExprNode::postDecrement:
            {
                UnaryExprNode *u = checked_cast<UnaryExprNode *>(p);
                Reference *lVal = SetupExprNode(env, phase, u->op, exprType);
                if (lVal)
                    lVal->emitPostDecBytecode(bCon, p->pos);
                else
                    reportError(Exception::semanticError, "PostDecrement needs an lValue", p->pos);
            }
            break;
        case ExprNode::preIncrement:
            {
                UnaryExprNode *u = checked_cast<UnaryExprNode *>(p);
                Reference *lVal = SetupExprNode(env, phase, u->op, exprType);
                if (lVal)
                    lVal->emitPreIncBytecode(bCon, p->pos);
                else
                    reportError(Exception::semanticError, "PreIncrement needs an lValue", p->pos);
            }
            break;
        case ExprNode::preDecrement:
            {
                UnaryExprNode *u = checked_cast<UnaryExprNode *>(p);
                Reference *lVal = SetupExprNode(env, phase, u->op, exprType);
                if (lVal)
                    lVal->emitPreDecBytecode(bCon, p->pos);
                else
                    reportError(Exception::semanticError, "PreDecrement needs an lValue", p->pos);
            }
            break;
        case ExprNode::index:
            {
                InvokeExprNode *i = checked_cast<InvokeExprNode *>(p);
                Reference *baseVal = SetupExprNode(env, phase, i->op, exprType);
                if (baseVal) baseVal->emitReadBytecode(bCon, p->pos);
                ExprPairList *ep = i->pairs;
                while (ep) {    // Validate has made sure there is only one, unnamed argument
                    Reference *argVal = SetupExprNode(env, phase, ep->value, exprType);
                    if (argVal) argVal->emitReadBytecode(bCon, p->pos);
                    ep = ep->next;
                }
                returnRef = new BracketReference();
            }
            break;
        case ExprNode::dot:
            {
                BinaryExprNode *b = checked_cast<BinaryExprNode *>(p);
                Reference *baseVal = SetupExprNode(env, phase, b->op1, exprType);
                if (baseVal) baseVal->emitReadBytecode(bCon, p->pos);

                if (b->op2->getKind() == ExprNode::identifier) {
                    IdentifierExprNode *i = checked_cast<IdentifierExprNode *>(b->op2);
                    if (*exprType) {
                        MemberDescriptor m2;
                        Multiname multiname(&i->name);
                        if (findLocalMember(*exprType, &multiname, ReadAccess, CompilePhase, &m2)) {
                            if (m2.ns) {
                                QualifiedName qname(m2.ns, multiname.name);
                                InstanceMember *m = findInstanceMember(*exprType, &qname, ReadAccess);
                                if (m->kind == InstanceMember::InstanceVariableKind)
                                    returnRef = new SlotReference(checked_cast<InstanceVariable *>(m)->slotIndex);
                            }
                        }
                    }
                    if (returnRef == NULL) {
                        returnRef = new DotReference(&i->name);
                        checked_cast<DotReference *>(returnRef)->propertyMultiname.addNamespace(cxt);
                    }
                } 
                else {
                    if (b->op2->getKind() == ExprNode::qualify) {
                        Reference *rVal = SetupExprNode(env, phase, b->op2, exprType);
                        ASSERT(rVal && checked_cast<LexicalReference *>(rVal));
                        returnRef = new DotReference(&((LexicalReference *)rVal)->variableMultiname);
                        checked_cast<DotReference *>(returnRef)->propertyMultiname.addNamespace(cxt);
                    }
                    // XXX else bracketRef...
                    else
                        NOT_REACHED("do we support these, or not?");
                }
            }
            break;
        case ExprNode::boolean:
            if (checked_cast<BooleanExprNode *>(p)->value) 
                bCon->emitOp(eTrue, p->pos);
            else 
                bCon->emitOp(eFalse, p->pos);
            break;
        case ExprNode::arrayLiteral:
            {
                int32 argCount = 0;
                PairListExprNode *plen = checked_cast<PairListExprNode *>(p);
                ExprPairList *e = plen->pairs;
                while (e) {
                    if (e->value) {
                        Reference *rVal = SetupExprNode(env, phase, e->value, exprType);
                        if (rVal) rVal->emitReadBytecode(bCon, p->pos);
                        argCount++;
                    }
                    e = e->next;
                }
                bCon->emitOp(eNewArray, p->pos, -argCount + 1);    // pop argCount args and push a new array
                bCon->addShort((uint16)argCount);
            }
            break;
        case ExprNode::objectLiteral:
            {
                int32 argCount = 0;
                PairListExprNode *plen = checked_cast<PairListExprNode *>(p);
                ExprPairList *e = plen->pairs;
                while (e) {
                    ASSERT(e->field && e->value);
                    Reference *rVal = SetupExprNode(env, phase, e->value, exprType);
                    if (rVal) rVal->emitReadBytecode(bCon, p->pos);
                    switch (e->field->getKind()) {
                    case ExprNode::identifier:
                        bCon->addString(&checked_cast<IdentifierExprNode *>(e->field)->name, p->pos);
                        break;
                    case ExprNode::string:
                        bCon->addString(checked_cast<StringExprNode *>(e->field)->str, p->pos);
                        break;
                    case ExprNode::number:
                        bCon->addString(engine->numberToString(&(checked_cast<NumberExprNode *>(e->field))->value), p->pos);
                        break;
                    default:
                        NOT_REACHED("bad field name");
                    }
                    argCount++;
                    e = e->next;
                }
                bCon->emitOp(eNewObject, p->pos, -argCount + 1);    // pop argCount args and push a new object
                bCon->addShort((uint16)argCount);
            }
            break;
        case ExprNode::Typeof:
            {
                UnaryExprNode *u = checked_cast<UnaryExprNode *>(p);
                Reference *rVal = SetupExprNode(env, phase, u->op, exprType);
                if (rVal) rVal->emitReadBytecode(bCon, p->pos);
                bCon->emitOp(eTypeof, p->pos);
            }
            break;
        case ExprNode::call:
            {
                InvokeExprNode *i = checked_cast<InvokeExprNode *>(p);
                Reference *rVal = SetupExprNode(env, phase, i->op, exprType);
                if (rVal) 
                    rVal->emitReadForInvokeBytecode(bCon, p->pos);
                else /* a call doesn't have to have an lValue to execute on, 
                      * but we use the value as it's own 'this' in that case. 
                      */
                    bCon->emitOp(eDup, p->pos);
                ExprPairList *args = i->pairs;
                uint16 argCount = 0;
                while (args) {
                    Reference *r = SetupExprNode(env, phase, args->value, exprType);
                    if (r) r->emitReadBytecode(bCon, p->pos);
                    argCount++;
                    args = args->next;
                }
                bCon->emitOp(eCall, p->pos, -(argCount + 2) + 1);    // pop argCount args, the base & function, and push a result
                bCon->addShort(argCount);
            }
            break;
        case ExprNode::New: 
            {
                // XXX why not?--> if (phase == CompilePhase) reportError(Exception::compileExpressionError, "Inappropriate compile time expression", p->pos);
                InvokeExprNode *i = checked_cast<InvokeExprNode *>(p);
                Reference *rVal = SetupExprNode(env, phase, i->op, exprType);
                if (rVal) rVal->emitReadBytecode(bCon, p->pos);
                ExprPairList *args = i->pairs;
                uint16 argCount = 0;
                while (args) {
                    Reference *r = SetupExprNode(env, phase, args->value, exprType);
                    if (r) r->emitReadBytecode(bCon, p->pos);
                    argCount++;
                    args = args->next;
                }
                bCon->emitOp(eNew, p->pos, -(argCount + 1) + 1);    // pop argCount args, the type/function, and push a result
                bCon->addShort(argCount);
            }
            break;
        case ExprNode::comma:
            {
                BinaryExprNode *b = checked_cast<BinaryExprNode *>(p);
                Reference *r = SetupExprNode(env, phase, b->op1, exprType);
                if (r) r->emitReadBytecode(bCon, p->pos);
                bCon->emitOp(ePopv, p->pos);
                returnRef = SetupExprNode(env, phase, b->op2, exprType);
            }
            break;
        case ExprNode::Instanceof:
            {
                BinaryExprNode *b = checked_cast<BinaryExprNode *>(p);
                Reference *rVal = SetupExprNode(env, phase, b->op1, exprType);
                if (rVal) rVal->emitReadBytecode(bCon, p->pos);
                rVal = SetupExprNode(env, phase, b->op2, exprType);
                if (rVal) rVal->emitReadBytecode(bCon, p->pos);
                bCon->emitOp(eInstanceof, p->pos);
            }
            break;
        case ExprNode::functionLiteral:
            {
                FunctionExprNode *f = checked_cast<FunctionExprNode *>(p);
                CompilationData *oldData = startCompilationUnit(f->function.fWrap->bCon, bCon->mSource, bCon->mSourceLocation);
                env->addFrame(f->function.fWrap->compileFrame);
                SetupStmt(env, phase, f->function.body);
                // XXX need to make sure that all paths lead to an exit of some kind
                bCon->emitOp(eReturnVoid, p->pos);
                env->removeTopFrame();
                restoreCompilationUnit(oldData);
            }
            break;
        default:
            NOT_REACHED("Not Yet Implemented");
        }
        return returnRef;
    }

/************************************************************************************
 *
 *  Environment
 *
 ************************************************************************************/

    // If env is from within a class's body, getEnclosingClass(env) returns the 
    // innermost such class; otherwise, it returns none.
    JS2Class *Environment::getEnclosingClass()
    {
        FrameListIterator fi = getBegin();
        while (fi != getEnd()) {
            if ((*fi)->kind == ClassKind)
                return checked_cast<JS2Class *>(*fi);
            fi++;
        }
        return NULL;
    }

    // Returns the most specific regional frame. A regional frame is either any frame other than 
    // a local block frame or a local block frame whose immediate enclosing frame is a class.
    FrameListIterator Environment::getRegionalFrame()
    {
        FrameListIterator fi = getBegin();
        while (fi != getEnd()) {
            if ((*fi)->kind != BlockKind)
                break;
            fi++;
        }
        if ((fi != getBegin()) && ((*fi)->kind == ClassKind))
            fi--;
        return fi;
    }

    // Returns the penultimate frame, either Package or Global
    Frame *Environment::getPackageOrGlobalFrame()
    {
        return *(getEnd() - 2);
    }

    // findThis returns the value of this. If allowPrototypeThis is true, allow this to be defined 
    // by either an instance member of a class or a prototype function. If allowPrototypeThis is 
    // false, allow this to be defined only by an instance member of a class.
    js2val Environment::findThis(bool allowPrototypeThis)
    {
        FrameListIterator fi = getBegin();
        while (fi != getEnd()) {
            Frame *pf = *fi;
            if ((pf->kind == ParameterKind)
                    && !JS2VAL_IS_NULL(checked_cast<ParameterFrame *>(pf)->thisObject))
                if (allowPrototypeThis || !checked_cast<ParameterFrame *>(pf)->prototype)
                    return checked_cast<ParameterFrame *>(pf)->thisObject;
            if (pf->kind == GlobalObjectKind)
                return OBJECT_TO_JS2VAL(pf);
            fi++;
        }
        return JS2VAL_VOID;
    }

    // Read the value of a lexical reference - it's an error if that reference
    // doesn't have a binding somewhere.
    // Attempt the read in each frame in the current environment, stopping at the
    // first succesful effort. If the property can't be found in any frame, it's 
    // an error.
    js2val Environment::lexicalRead(JS2Metadata *meta, Multiname *multiname, Phase phase)
    {
        LookupKind lookup(true, findThis(false));
        FrameListIterator fi = getBegin();
        while (fi != getEnd()) {
            js2val rval;    // XXX gc?
            if (meta->readProperty(*fi, multiname, &lookup, phase, &rval))
                return rval;
            fi++;
        }
        meta->reportError(Exception::referenceError, "{0} is undefined", meta->engine->errorPos(), multiname->name);
        return JS2VAL_VOID;
    }

    // Attempt the write in the top frame in the current environment - if the property
    // exists, then fine. Otherwise create the property there.
    void Environment::lexicalWrite(JS2Metadata *meta, Multiname *multiname, js2val newValue, bool createIfMissing, Phase phase)
    {
        LookupKind lookup(true, findThis(false));
        FrameListIterator fi = getBegin();
        while (fi != getEnd()) {
            if (meta->writeProperty(*fi, multiname, &lookup, false, newValue, phase, false))
                return;
            fi++;
        }
        if (createIfMissing) {
            Frame *pf = getPackageOrGlobalFrame();
            if (pf->kind == GlobalObjectKind) {
                if (meta->writeProperty(pf, multiname, &lookup, true, newValue, phase, false))
                    return;
            }
        }
        meta->reportError(Exception::referenceError, "{0} is undefined", meta->engine->errorPos(), multiname->name);
    }

    // Initialize a variable - it might not be in the immediate frame, because of hoisting
    // but it had darn well better be in the environment somewhere.
    void Environment::lexicalInit(JS2Metadata *meta, Multiname *multiname, js2val newValue)
    {
        LookupKind lookup(true, findThis(false));
        FrameListIterator fi = getBegin();
        while (fi != getEnd()) {
            if (meta->writeProperty(*fi, multiname, &lookup, false, newValue, RunPhase, true))
                return;
            fi++;
        }
        ASSERT(false);
    }

    // Delete the named property in the current environment, return true if the property
    // can't be found, or the result of the deleteProperty call if it was found.
    bool Environment::lexicalDelete(JS2Metadata *meta, Multiname *multiname, Phase phase)
    {
        LookupKind lookup(true, findThis(false));
        FrameListIterator fi = getBegin();
        while (fi != getEnd()) {
            bool result;
            if (meta->deleteProperty(*fi, multiname, &lookup, phase, &result))
                return result;
            fi++;
        }
        return true;
    }

    // Clone the pluralFrame bindings into the singularFrame, instantiating new members for each binding
    void Environment::instantiateFrame(NonWithFrame *pluralFrame, NonWithFrame *singularFrame)
    {
        singularFrame->localBindings.clear();

        for (LocalBindingIterator bi = pluralFrame->localBindings.begin(), bend = pluralFrame->localBindings.end(); (bi != bend); bi++) {
            LocalBindingEntry *lbe = *bi;
            lbe->clear();
        }
        for (LocalBindingIterator bi2 = pluralFrame->localBindings.begin(), bend2 = pluralFrame->localBindings.end(); (bi2 != bend2); bi2++) {
            LocalBindingEntry *lbe = *bi2;
            singularFrame->localBindings.insert(lbe->name, lbe->clone());
        }

    }

    // need to mark all the frames in the environment - otherwise a marked frame that
    // came initially from the bytecodeContainer may prevent the markChildren call
    // from finding frames further down the list.
    void Environment::markChildren()
    { 
        FrameListIterator fi = getBegin();
        while (fi != getEnd()) {
            GCMARKOBJECT(*fi)
            fi++;
        }
    }


/************************************************************************************
 *
 *  Context
 *
 ************************************************************************************/

    // clone a context
    Context::Context(Context *cxt) : strict(cxt->strict), openNamespaces(cxt->openNamespaces)
    {
        ASSERT(false);  // ?? used ??
    }


/************************************************************************************
 *
 *  Multiname
 *
 ************************************************************************************/

    // return true if the given namespace is on the namespace list
    bool Multiname::listContains(Namespace *nameSpace)
    { 
        if (nsList->empty())
            return true;
        for (NamespaceListIterator n = nsList->begin(), end = nsList->end(); (n != end); n++) {
            if (*n == nameSpace)
                return true;
        }
        return false;
    }

    // add all the open namespaces from the given context
    void Multiname::addNamespace(Context &cxt)    
    { 
        addNamespace(&cxt.openNamespaces); 
    }


    // add every namespace from the list to this Multiname
    void Multiname::addNamespace(NamespaceList *ns)
    {
        for (NamespaceListIterator nli = ns->begin(), end = ns->end();
                (nli != end); nli++)
            nsList->push_back(*nli);
    }

    // gc-mark all contained JS2Objects and visit contained structures to do likewise
    void Multiname::markChildren()
    {
        for (NamespaceListIterator n = nsList->begin(), end = nsList->end(); (n != end); n++) {
            GCMARKOBJECT(*n)
        }
        if (name) JS2Object::mark(name);
    }

/************************************************************************************
 *
 *  JS2Metadata
 *
 ************************************************************************************/

    // - Define namespaces::id (for all namespaces or at least 'public') in the top frame 
    //     unless it's there already. 
    // - If the binding exists (not forbidden) in lower frames in the regional environment, it's an error.
    // - Define a forbidden binding in all the lower frames.
    // 
    Multiname *JS2Metadata::defineLocalMember(Environment *env, const String *id, NamespaceList *namespaces, 
                                                Attribute::OverrideModifier overrideMod, bool xplicit, Access access,
                                                LocalMember *m, size_t pos)
    {
        NamespaceList publicNamespaceList;

        FrameListIterator fi = env->getBegin();
        NonWithFrame *localFrame = checked_cast<NonWithFrame *>(*fi);
        if ((overrideMod != Attribute::NoOverride) || (xplicit && localFrame->kind != PackageKind))
            reportError(Exception::definitionError, "Illegal definition", pos);
        if ((namespaces == NULL) || namespaces->empty()) {
            publicNamespaceList.push_back(publicNamespace);
            namespaces = &publicNamespaceList;
        }
        Multiname *mn = new Multiname(id);
        mn->addNamespace(namespaces);

        // Search the local frame for an overlapping definition
        LocalBindingEntry **lbeP = localFrame->localBindings[*id];
        if (lbeP) {
            for (LocalBindingEntry::NS_Iterator i = (*lbeP)->begin(), end = (*lbeP)->end(); (i != end); i++) {
                LocalBindingEntry::NamespaceBinding &ns = *i;
                if ((ns.second->accesses & access) && mn->listContains(ns.first))
                    reportError(Exception::definitionError, "Duplicate definition {0}", pos, id);
            }
        }

        // Check all frames below the current - up to the RegionalFrame - for a non-forbidden definition
        Frame *regionalFrame = *env->getRegionalFrame();
        if (localFrame != regionalFrame) {
            // The frame iterator is pointing at the top of the environment's
            // frame list, start at the one below that and continue to the frame
            // returned by 'getRegionalFrame()'.
            Frame *fr = *++fi;
            while (true) {
                if (fr->kind != WithFrameKind) {
                    NonWithFrame *nwfr = checked_cast<NonWithFrame *>(fr);
                    LocalBindingEntry **rbeP = nwfr->localBindings[*id];
                    if (rbeP) {
                        for (LocalBindingEntry::NS_Iterator i = (*rbeP)->begin(), end = (*rbeP)->end(); (i != end); i++) {
                            LocalBindingEntry::NamespaceBinding &ns = *i;
                            if ((ns.second->accesses & access) 
                                    && (ns.second->content->kind != LocalMember::Forbidden)
                                    && mn->listContains(ns.first))
                                reportError(Exception::definitionError, "Duplicate definition {0}", pos, id);
                        }
                    }
                }
                if (fr == regionalFrame)
                    break;
                fr = *++fi;
                ASSERT(fr);
            }
        }

        // Now insert the id, via all it's namespaces into the local frame
        LocalBindingEntry *lbe;
        if (lbeP == NULL) {
            lbe = new LocalBindingEntry(*id);
            localFrame->localBindings.insert(*id, lbe);
        }
        else
            lbe = *lbeP;
        for (NamespaceListIterator nli = mn->nsList->begin(), nlend = mn->nsList->end(); (nli != nlend); nli++) {
            LocalBinding *new_b = new LocalBinding(access, m);
            lbe->bindingList.push_back(LocalBindingEntry::NamespaceBinding(*nli, new_b));
        }
        // Mark the bindings of multiname as Forbidden in all non-innermost frames in the current
        // region if they haven't been marked as such already.
        if (localFrame != regionalFrame) {
            fi = env->getBegin();
            Frame *fr = *++fi;
            while (true) {
                if (fr->kind != WithFrameKind) {
                    NonWithFrame *nwfr = checked_cast<NonWithFrame *>(fr);
                    for (NamespaceListIterator nli = mn->nsList->begin(), nlend = mn->nsList->end(); (nli != nlend); nli++) {
                        bool foundEntry = false;
                        LocalBindingEntry **rbeP = nwfr->localBindings[*id];
                        if (rbeP) {
                            for (LocalBindingEntry::NS_Iterator i = (*rbeP)->begin(), end = (*rbeP)->end(); (i != end); i++) {
                                LocalBindingEntry::NamespaceBinding &ns = *i;
                                if ((ns.second->accesses & access) && (ns.first == *nli)) {
                                    ASSERT(ns.second->content->kind == LocalMember::Forbidden);
                                    foundEntry = true;
                                    break;
                                }
                            }
                        }
                        if (!foundEntry) {
                            LocalBindingEntry *rbe = new LocalBindingEntry(*id);
                            nwfr->localBindings.insert(*id, rbe);
                            LocalBinding *new_b = new LocalBinding(access, forbiddenMember);
                            rbe->bindingList.push_back(LocalBindingEntry::NamespaceBinding(*nli, new_b));
                        }
                    }
                }
                if (fr == regionalFrame)
                    break;
                fr = *++fi;
            }
        }
        return mn;
    }

    // Look through 'c' and all it's super classes for an identifier 
    // matching the qualified name and access.
    InstanceMember *JS2Metadata::findInstanceMember(JS2Class *c, QualifiedName *qname, Access access)
    {
        if (qname == NULL)
            return NULL;
        JS2Class *s = c;
        while (s) {
            InstanceBindingEntry **ibeP = c->instanceBindings[*qname->id];
            if (ibeP) {
                for (InstanceBindingEntry::NS_Iterator i = (*ibeP)->begin(), end = (*ibeP)->end(); (i != end); i++) {
                    InstanceBindingEntry::NamespaceBinding &ns = *i;
                    if ((ns.second->accesses & access) && (ns.first == qname->nameSpace)) {
                        return ns.second->content;
                    }
                }
            }
            s = s->super;
        }
        return NULL;
    }

    // Examine class 'c' and find all instance members that would be overridden
    // by 'id' in any of the given namespaces.
    OverrideStatus *JS2Metadata::searchForOverrides(JS2Class *c, const String *id, NamespaceList *namespaces, Access access, size_t pos)
    {
        OverrideStatus *os = new OverrideStatus(NULL, id);
        for (NamespaceListIterator ns = namespaces->begin(), end = namespaces->end(); (ns != end); ns++) {
            QualifiedName qname(*ns, id);
            InstanceMember *m = findInstanceMember(c, &qname, access);
            if (m) {
                os->multiname.addNamespace(*ns);
                if (os->overriddenMember == NULL)
                    os->overriddenMember = m;
                else
                    if (os->overriddenMember != m)  // different instance members by same id
                        reportError(Exception::definitionError, "Illegal override", pos);
            }
        }
        return os;
    }

    // Find the possible override conflicts that arise from the given id and namespaces
    // Fall back on the currently open namespace list if no others are specified.
    OverrideStatus *JS2Metadata::resolveOverrides(JS2Class *c, Context *cxt, const String *id, NamespaceList *namespaces, Access access, bool expectMethod, size_t pos)
    {
        OverrideStatus *os = NULL;
        if ((namespaces == NULL) || namespaces->empty()) {
            os = searchForOverrides(c, id, &cxt->openNamespaces, access, pos);
            if (os->overriddenMember == NULL) {
                ASSERT(os->multiname.nsList->empty());
                os->multiname.addNamespace(publicNamespace);
            }
        }
        else {
            OverrideStatus *os2 = searchForOverrides(c, id, namespaces, access, pos);
            if (os2->overriddenMember == NULL) {
                OverrideStatus *os3 = searchForOverrides(c, id, &cxt->openNamespaces, access, pos);
                if (os3->overriddenMember == NULL) {
                    os = new OverrideStatus(NULL, id);
                    os->multiname.addNamespace(namespaces);
                }
                else {
                    os = new OverrideStatus(POTENTIAL_CONFLICT, id);    // Didn't find the member with a specified namespace, but did with
                                                                        // the use'd ones. That'll be an error unless the override is 
                                                                        // disallowed (in defineInstanceMember below)
                    os->multiname.addNamespace(namespaces);
                }
                delete os3;
                delete os2;
            }
            else {
                os = os2;
                os->multiname.addNamespace(namespaces);
            }
        }
        // For all the discovered possible overrides, make sure the member doesn't already exist in the class
        for (NamespaceListIterator nli = os->multiname.nsList->begin(), nlend = os->multiname.nsList->end(); (nli != nlend); nli++) {
            InstanceBindingEntry **ibeP = c->instanceBindings[*id];
            if (ibeP) {
                for (InstanceBindingEntry::NS_Iterator i = (*ibeP)->begin(), end = (*ibeP)->end(); (i != end); i++) {
                    InstanceBindingEntry::NamespaceBinding &ns = *i;
                    if ((ns.second->accesses & access) && (ns.first == *nli))
                        reportError(Exception::definitionError, "Illegal override", pos);
                }
            }
        }
        // Make sure we're getting what we expected
        if (expectMethod) {
            if (os->overriddenMember && (os->overriddenMember != POTENTIAL_CONFLICT) && (os->overriddenMember->kind != InstanceMember::InstanceMethodKind))
                reportError(Exception::definitionError, "Illegal override, expected method", pos);
        }
        else {
            if (os->overriddenMember && (os->overriddenMember != POTENTIAL_CONFLICT) && (os->overriddenMember->kind == InstanceMember::InstanceMethodKind))
                reportError(Exception::definitionError, "Illegal override, didn't expect method", pos);
        }

        return os;
    }

    // Define an instance member in the class. Verify that, if any overriding is happening, it's legal. The result pair indicates
    // the members being overridden.
    OverrideStatusPair *JS2Metadata::defineInstanceMember(JS2Class *c, Context *cxt, const String *id, NamespaceList *namespaces, Attribute::OverrideModifier overrideMod, bool xplicit, Access access, InstanceMember *m, size_t pos)
    {
        OverrideStatus *readStatus;
        OverrideStatus *writeStatus;
        if (xplicit)
            reportError(Exception::definitionError, "Illegal use of explicit", pos);

        if (access & ReadAccess)
            readStatus = resolveOverrides(c, cxt, id, namespaces, ReadAccess, (m->kind == InstanceMember::InstanceMethodKind), pos);
        else
            readStatus = new OverrideStatus(NULL, id);

        if (access & WriteAccess)
            writeStatus = resolveOverrides(c, cxt, id, namespaces, WriteAccess, (m->kind == InstanceMember::InstanceMethodKind), pos);
        else
            writeStatus = new OverrideStatus(NULL, id);

        if ((readStatus->overriddenMember && (readStatus->overriddenMember != POTENTIAL_CONFLICT))
                || (writeStatus->overriddenMember && (writeStatus->overriddenMember != POTENTIAL_CONFLICT))) {
            if ((overrideMod != Attribute::DoOverride) && (overrideMod != Attribute::OverrideUndefined))
                reportError(Exception::definitionError, "Illegal override", pos);
        }
        else {
            if ((readStatus->overriddenMember == POTENTIAL_CONFLICT) || (writeStatus->overriddenMember == POTENTIAL_CONFLICT)) {
                if ((overrideMod != Attribute::DontOverride) && (overrideMod != Attribute::OverrideUndefined))
                    reportError(Exception::definitionError, "Illegal override", pos);
            }
        }

        NamespaceListIterator nli, nlend;
        InstanceBindingEntry **ibeP = c->instanceBindings[*id];
        InstanceBindingEntry *ibe;
        if (ibeP == NULL) {
            ibe = new InstanceBindingEntry(*id);
            c->instanceBindings.insert(*id, ibe);
        }
        else
            ibe = *ibeP;
        for (nli = readStatus->multiname.nsList->begin(), nlend = readStatus->multiname.nsList->end(); (nli != nlend); nli++) {
            InstanceBinding *ib = new InstanceBinding(ReadAccess, m);
            ibe->bindingList.push_back(InstanceBindingEntry::NamespaceBinding(*nli, ib));
        }
        
        for (nli = writeStatus->multiname.nsList->begin(), nlend = writeStatus->multiname.nsList->end(); (nli != nlend); nli++) {
            InstanceBinding *ib = new InstanceBinding(ReadAccess, m);
            ibe->bindingList.push_back(InstanceBindingEntry::NamespaceBinding(*nli, ib));
        }
        
        return new OverrideStatusPair(readStatus, writeStatus);;
    }

    // Define a hoisted var in the current frame (either Global or a Function)
    // defineHoistedVar(env, id, initialValue) defines a hoisted variable with the name id in the environment env. 
    // Hoisted variables are hoisted to the global or enclosing function scope. Multiple hoisted variables may be 
    // defined in the same scope, but they may not coexist with non-hoisted variables with the same name. A hoisted 
    // variable can be defined using either a var or a function statement. If it is defined using var, then initialValue
    // is always undefined (if the var statement has an initialiser, then the variable's value will be written later 
    // when the var statement is executed). If it is defined using function, then initialValue must be a function 
    // instance or open instance. According to rules inherited from ECMAScript Edition 3, if there are multiple 
    // definitions of a hoisted variable, then the initial value of that variable is undefined if none of the definitions
    // is a function definition; otherwise, the initial value is the last function definition.
    DynamicVariable *JS2Metadata::defineHoistedVar(Environment *env, const String *id, StmtNode *p)
    {
        DynamicVariable *result = NULL;
        QualifiedName qName(publicNamespace, id);
        FrameListIterator regionalFrameMark = env->getRegionalFrame();
        // XXX can the regionalFrame be a WithFrame?
        NonWithFrame *regionalFrame = checked_cast<NonWithFrame *>(*regionalFrameMark);
        ASSERT((regionalFrame->kind == GlobalObjectKind) || (regionalFrame->kind == ParameterKind));
          
        // run through all the existing bindings, to see if this variable already exists.
        LocalBindingEntry **lbeP = regionalFrame->localBindings[*id];
        if (lbeP) {
            for (LocalBindingEntry::NS_Iterator i = (*lbeP)->begin(), end = (*lbeP)->end(); (i != end); i++) {
                LocalBindingEntry::NamespaceBinding &ns = *i;
                if (ns.first == publicNamespace) {
                    if (ns.second->content->kind != LocalMember::DynamicVariableKind)
                        reportError(Exception::definitionError, "Duplicate definition {0}", p->pos, id);
                    else {
                        if (result)
                            reportError(Exception::definitionError, "Duplicate definition {0}", p->pos, id);
                        else
                            result = checked_cast<DynamicVariable *>(ns.second->content);
                    }
                }
            }
        }
        
        if (result == NULL) {
            if (regionalFrame->kind == GlobalObjectKind) {
                GlobalObject *gObj = checked_cast<GlobalObject *>(regionalFrame);
                DynamicPropertyBinding **dpbP = gObj->dynamicProperties[*id];
                if (dpbP)
                    reportError(Exception::definitionError, "Duplicate definition {0}", p->pos, id);
            }
            else { // ParameterFrame didn't have any bindings, scan the preceding 
                   // frame (should be the outermost function local block)
                regionalFrame = checked_cast<NonWithFrame *>(*(regionalFrameMark - 1));
                LocalBindingEntry **rbeP = regionalFrame->localBindings[*id];
                if (rbeP) {
                    for (LocalBindingEntry::NS_Iterator i = (*rbeP)->begin(), end = (*rbeP)->end(); (i != end); i++) {
                        LocalBindingEntry::NamespaceBinding &ns = *i;
                        if (ns.first == publicNamespace) {
                            if (ns.second->content->kind != LocalMember::DynamicVariableKind)
                                reportError(Exception::definitionError, "Duplicate definition {0}", p->pos, id);
                            else {
                                if (result)
                                    reportError(Exception::definitionError, "Duplicate definition {0}", p->pos, id);
                                else
                                    result = checked_cast<DynamicVariable *>(ns.second->content);
                            }
                        }
                    }
                }
            }
            if (result == NULL) {
                LocalBindingEntry *lbe;
                if (lbeP == NULL) {
                    lbe = new LocalBindingEntry(*id);
                    regionalFrame->localBindings.insert(*id, lbe);
                }
                else
                    lbe = *lbeP;
                result = new DynamicVariable();
                LocalBinding *sb = new LocalBinding(ReadWriteAccess, result);
                lbe->bindingList.push_back(LocalBindingEntry::NamespaceBinding(publicNamespace, sb));
            }
        }
        //... A hoisted binding of the same var already exists, so there is no need to create another one
        //    The initial value of function variables will be set by the caller to the 'most recent' value.
        return result;
    }

    static js2val GlobalObject_isNaN(JS2Metadata *meta, const js2val /* thisValue */, js2val argv[], uint32 /* argc */)
    {
        float64 d = meta->toFloat64(argv[0]);
        return BOOLEAN_TO_JS2VAL(JSDOUBLE_IS_NaN(d));
    }

    static js2val GlobalObject_toString(JS2Metadata *meta, const js2val /* thisValue */, js2val argv[], uint32 /* argc */)
    {
        return STRING_TO_JS2VAL(meta->engine->allocString("[object global]"));
    }

    static js2val GlobalObject_eval(JS2Metadata *meta, const js2val /* thisValue */, js2val argv[], uint32 argc)
    {
        if (!JS2VAL_IS_STRING(argv[0]))
            return argv[0];
        return meta->readEvalString(*meta->toString(argv[0]), widenCString("Eval Source"));
    }

#define JS7_ISHEX(c)    ((c) < 128 && isxdigit(c))
#define JS7_UNHEX(c)    (uint32)(isdigit(c) ? (c) - '0' : 10 + tolower(c) - 'a')

    /* See ECMA-262 15.1.2.5 */
    static js2val GlobalObject_unescape(JS2Metadata *meta, const js2val /* thisValue */, js2val argv[], uint32 argc)
    {
        const String *str = meta->toString(argv[0]);
        const char16 *chars = str->data();
        uint32 length = str->length();

        /* Don't bother allocating less space for the new string. */
        char16 *newchars = new char16[length + 1];
    
        uint32 ni = 0;
        uint32 i = 0;
        char16 ch;
        while (i < length) {
            ch = chars[i++];
            if (ch == '%') {
                if (i + 1 < length &&
                    JS7_ISHEX(chars[i]) && JS7_ISHEX(chars[i + 1]))
                {
                    ch = JS7_UNHEX(chars[i]) * 16 + JS7_UNHEX(chars[i + 1]);
                    i += 2;
                } else if (i + 4 < length && chars[i] == 'u' &&
                           JS7_ISHEX(chars[i + 1]) && JS7_ISHEX(chars[i + 2]) &&
                           JS7_ISHEX(chars[i + 3]) && JS7_ISHEX(chars[i + 4]))
                {
                    ch = (((((JS7_UNHEX(chars[i + 1]) << 4)
                            + JS7_UNHEX(chars[i + 2])) << 4)
                          + JS7_UNHEX(chars[i + 3])) << 4)
                        + JS7_UNHEX(chars[i + 4]);
                    i += 5;
                }
            }
            newchars[ni++] = ch;
        }
        newchars[ni] = 0;
        
        return STRING_TO_JS2VAL(meta->engine->allocStringPtr(&meta->world.identifiers[newchars]));
    }

    // Taken from jsstr.c...
/*
 * Stuff to emulate the old libmocha escape, which took a second argument
 * giving the type of escape to perform.  Retained for compatibility, and
 * copied here to avoid reliance on net.h, mkparse.c/NET_EscapeBytes.
 */

#define URL_XALPHAS     ((uint8) 1)
#define URL_XPALPHAS    ((uint8) 2)
#define URL_PATH        ((uint8) 4)

static const uint8 urlCharType[256] =
/*      Bit 0           xalpha          -- the alphas
 *      Bit 1           xpalpha         -- as xalpha but
 *                             converts spaces to plus and plus to %20
 *      Bit 2 ...       path            -- as xalphas but doesn't escape '/'
 */
    /*   0 1 2 3 4 5 6 7 8 9 A B C D E F */
    {    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,       /* 0x */
         0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,       /* 1x */
         0,0,0,0,0,0,0,0,0,0,7,4,0,7,7,4,       /* 2x   !"#$%&'()*+,-./  */
         7,7,7,7,7,7,7,7,7,7,0,0,0,0,0,0,       /* 3x  0123456789:;<=>?  */
         7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,       /* 4x  @ABCDEFGHIJKLMNO  */
         7,7,7,7,7,7,7,7,7,7,7,0,0,0,0,7,       /* 5X  PQRSTUVWXYZ[\]^_  */
         0,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,       /* 6x  `abcdefghijklmno  */
         7,7,7,7,7,7,7,7,7,7,7,0,0,0,0,0,       /* 7X  pqrstuvwxyz{\}~  DEL */
         0, };

/* This matches the ECMA escape set when mask is 7 (default.) */

#define IS_OK(C, mask) (urlCharType[((uint8) (C))] & (mask))

    /* See ECMA-262 15.1.2.4. */
    static js2val GlobalObject_escape(JS2Metadata *meta, const js2val /* thisValue */, js2val argv[], uint32 argc)
    {
        uint32 newlength;
        char16 ch;
        const char digits[] = {'0', '1', '2', '3', '4', '5', '6', '7',
                               '8', '9', 'A', 'B', 'C', 'D', 'E', 'F' };

        int32 mask = URL_XALPHAS | URL_XPALPHAS | URL_PATH;
        if (argc > 1) {
            float64 d = meta->toFloat64(argv[1]);
            if (!JSDOUBLE_IS_FINITE(d) ||
                (mask = (int32)d) != d ||
                mask & ~(URL_XALPHAS | URL_XPALPHAS | URL_PATH))
            {
                meta->reportError(Exception::badValueError,  "Need integral non-zero mask for escape", meta->engine->errorPos());
            }
        }

        const String *str = meta->toString(argv[0]);
        const char16 *chars = str->data();
        uint32 length = newlength = str->length();

        /* Take a first pass and see how big the result string will need to be. */
        uint32 i;
        for (i = 0; i < length; i++) {
            if ((ch = chars[i]) < 128 && IS_OK(ch, mask))
                continue;
            if (ch < 256) {
                if (mask == URL_XPALPHAS && ch == ' ')
                    continue;   /* The character will be encoded as '+' */
                newlength += 2; /* The character will be encoded as %XX */
            } else {
                newlength += 5; /* The character will be encoded as %uXXXX */
            }
        }

        char16 *newchars = new char16[newlength + 1];
        uint32 ni;
        for (i = 0, ni = 0; i < length; i++) {
            if ((ch = chars[i]) < 128 && IS_OK(ch, mask)) {
                newchars[ni++] = ch;
            } else if (ch < 256) {
                if (mask == URL_XPALPHAS && ch == ' ') {
                    newchars[ni++] = '+'; /* convert spaces to pluses */
                } else {
                    newchars[ni++] = '%';
                    newchars[ni++] = digits[ch >> 4];
                    newchars[ni++] = digits[ch & 0xF];
                }
            } else {
                newchars[ni++] = '%';
                newchars[ni++] = 'u';
                newchars[ni++] = digits[ch >> 12];
                newchars[ni++] = digits[(ch & 0xF00) >> 8];
                newchars[ni++] = digits[(ch & 0xF0) >> 4];
                newchars[ni++] = digits[ch & 0xF];
            }
        }
        ASSERT(ni == newlength);
        newchars[newlength] = 0;

        return STRING_TO_JS2VAL(meta->engine->allocStringPtr(&meta->world.identifiers[newchars]));
    }

    static js2val GlobalObject_parseInt(JS2Metadata *meta, const js2val /* thisValue */, js2val argv[], uint32 argc)
    {
        const String *str = meta->toString(argv[0]);
        const char16 *chars = str->data();
        uint32 length = str->length();
        const char16 *numEnd;
        uint base = 10;
        
        if (argc > 1) {
            float64 d = meta->toFloat64(argv[1]);
            if (!JSDOUBLE_IS_FINITE(d) || ((base = (int32)d) != d))
                return meta->engine->nanValue;
            if (base == 0)
                base = 10;
            else
                if ((base < 2) || (base > 36))
                    return meta->engine->nanValue;
        }
        
        return meta->engine->allocNumber(stringToInteger(chars, chars + length, numEnd, base));
    }

    void JS2Metadata::addGlobalObjectFunction(char *name, NativeCode *code, uint32 length)
    {
        SimpleInstance *fInst = new SimpleInstance(functionClass);
        fInst->fWrap = new FunctionWrapper(true, new ParameterFrame(JS2VAL_VOID, true), code, env);
        writeDynamicProperty(glob, new Multiname(&world.identifiers[name], publicNamespace), true, OBJECT_TO_JS2VAL(fInst), RunPhase);
        fInst->writeProperty(this, engine->length_StringAtom, INT_TO_JS2VAL(length), DynamicPropertyValue::READONLY);
    }

    static js2val Object_toString(JS2Metadata *meta, const js2val thisValue, js2val /* argv */ [], uint32 /* argc */)
    {
        ASSERT(JS2VAL_IS_OBJECT(thisValue));
        JS2Object *obj = JS2VAL_TO_OBJECT(thisValue);
        if (obj->kind == GlobalObjectKind) {
            // special case this for now, ECMA3 test sanity...
            return GlobalObject_toString(meta, thisValue, NULL, 0);
        }
        else {
            // XXX insist on Prototype instances, but is toString going to be more
            // generic - a member of the class Object? and hence available to all
            // instances?
            JS2Class *type = (checked_cast<PrototypeInstance *>(obj))->type;
        
            // XXX objectType returns the ECMA4 type, not the [[class]] value, so returns class 'Prototype' for ECMA3 objects
            //        JS2Class *type = meta->objectType(thisValue);

            String s = "[object " + *type->getName() + "]";
            return STRING_TO_JS2VAL(meta->engine->allocString(s));
        }
    }
    
#define MAKEBUILTINCLASS(c, super, dynamic, allowNull, final, name, defaultVal) c = new JS2Class(super, NULL, new Namespace(engine->private_StringAtom), dynamic, allowNull, final, name); c->complete = true; c->defaultValue = defaultVal;

    JS2Metadata::JS2Metadata(World &world) : JS2Object(MetaDataKind),
        world(world),
        engine(new JS2Engine(world)),
        publicNamespace(new Namespace(engine->public_StringAtom)),
        mn1(new Multiname(NULL, publicNamespace)),
        mn2(new Multiname(NULL, publicNamespace)),
        bCon(new BytecodeContainer()),
        glob(new GlobalObject(world)),
        env(new Environment(new MetaData::SystemFrame(), glob)),
        flags(JS1),
        showTrees(false)
    {
        engine->meta = this;

        JS2Object::addRoot(&mn1);
        JS2Object::addRoot(&mn2);

        cxt.openNamespaces.clear();
        cxt.openNamespaces.push_back(publicNamespace);

        MAKEBUILTINCLASS(objectClass, NULL, false, true, false, engine->Object_StringAtom, JS2VAL_VOID);
        MAKEBUILTINCLASS(undefinedClass, objectClass, false, false, true, engine->undefined_StringAtom, JS2VAL_VOID);
        MAKEBUILTINCLASS(nullClass, objectClass, false, true, true, engine->null_StringAtom, JS2VAL_NULL);
        MAKEBUILTINCLASS(booleanClass, objectClass, false, false, true, engine->allocStringPtr(&world.identifiers["Boolean"]), JS2VAL_FALSE);
        MAKEBUILTINCLASS(generalNumberClass, objectClass, false, false, false, engine->allocStringPtr(&world.identifiers["general number"]), engine->nanValue);
        MAKEBUILTINCLASS(numberClass, generalNumberClass, false, false, true, engine->allocStringPtr(&world.identifiers["Number"]), engine->nanValue);
        MAKEBUILTINCLASS(characterClass, objectClass, false, false, true, engine->allocStringPtr(&world.identifiers["Character"]), JS2VAL_ZERO);
        MAKEBUILTINCLASS(stringClass, objectClass, false, false, true, engine->allocStringPtr(&world.identifiers["String"]), JS2VAL_NULL);
        MAKEBUILTINCLASS(namespaceClass, objectClass, false, true, true, engine->allocStringPtr(&world.identifiers["namespace"]), JS2VAL_NULL);
        MAKEBUILTINCLASS(attributeClass, objectClass, false, true, true, engine->allocStringPtr(&world.identifiers["attribute"]), JS2VAL_NULL);
        MAKEBUILTINCLASS(classClass, objectClass, false, true, true, engine->allocStringPtr(&world.identifiers["Class"]), JS2VAL_NULL);
        MAKEBUILTINCLASS(functionClass, objectClass, true, true, true, engine->Function_StringAtom, JS2VAL_NULL);
        MAKEBUILTINCLASS(prototypeClass, objectClass, true, true, true, engine->allocStringPtr(&world.identifiers["prototype"]), JS2VAL_NULL);
        MAKEBUILTINCLASS(packageClass, objectClass, true, true, true, engine->allocStringPtr(&world.identifiers["Package"]), JS2VAL_NULL);

        


        // A 'forbidden' member, used to mark hidden bindings
        forbiddenMember = new LocalMember(Member::Forbidden, true);

        // needed for class instance variables etc...
        NamespaceList publicNamespaceList;
        publicNamespaceList.push_back(publicNamespace);
        Variable *v;

        
        
// XXX Built-in Attributes... XXX 
/*
XXX see EvalAttributeExpression, where identifiers are being handled for now...

        CompoundAttribute *attr = new CompoundAttribute();
        attr->dynamic = true;
        v = new Variable(attributeClass, OBJECT_TO_JS2VAL(attr), true);
        defineLocalMember(env, &world.identifiers["dynamic"], &publicNamespaceList, Attribute::NoOverride, false, ReadWriteAccess, v, 0);
*/

/*** ECMA 3  Global Object ***/
        // Non-function properties of the global object : 'undefined', 'NaN' and 'Infinity'
        writeDynamicProperty(glob, new Multiname(engine->undefined_StringAtom, publicNamespace), true, JS2VAL_UNDEFINED, RunPhase);
        writeDynamicProperty(glob, new Multiname(&world.identifiers["NaN"], publicNamespace), true, engine->nanValue, RunPhase);
        writeDynamicProperty(glob, new Multiname(&world.identifiers["Infinity"], publicNamespace), true, engine->posInfValue, RunPhase);
        // XXX add 'version()' 
        writeDynamicProperty(glob, new Multiname(&world.identifiers["version"], publicNamespace), true, INT_TO_JS2VAL(0), RunPhase);
        // Function properties of the global object 
        addGlobalObjectFunction("isNaN", GlobalObject_isNaN, 1);
        addGlobalObjectFunction("eval", GlobalObject_eval, 1);
        addGlobalObjectFunction("toString", GlobalObject_toString, 1);
        addGlobalObjectFunction("unescape", GlobalObject_unescape, 1);
        addGlobalObjectFunction("escape", GlobalObject_escape, 1);
        addGlobalObjectFunction("parseInt", GlobalObject_parseInt, 2);


/*** ECMA 3  Object Class ***/
        v = new Variable(classClass, OBJECT_TO_JS2VAL(objectClass), true);
        defineLocalMember(env, &world.identifiers["Object"], &publicNamespaceList, Attribute::NoOverride, false, ReadWriteAccess, v, 0);
        // Function properties of the Object prototype object
        objectClass->prototype = new PrototypeInstance(this, NULL, objectClass);
        // Adding "prototype" as a static member of the class - not a dynamic property
        env->addFrame(objectClass);
            v = new Variable(objectClass, OBJECT_TO_JS2VAL(objectClass->prototype), true);
            defineLocalMember(env, engine->prototype_StringAtom, &publicNamespaceList, Attribute::NoOverride, false, ReadWriteAccess, v, 0);
        env->removeTopFrame();

/*** ECMA 3  Function Class ***/
// Need this initialized early, as subsequent FunctionInstances need the Function.prototype value
        v = new Variable(classClass, OBJECT_TO_JS2VAL(functionClass), true);
        defineLocalMember(env, &world.identifiers["Function"], &publicNamespaceList, Attribute::NoOverride, false, ReadWriteAccess, v, 0);
        initFunctionObject(this);

// Adding 'toString' to the Object.prototype XXX Or make this a static class member?
        FunctionInstance *fInst = new FunctionInstance(this, functionClass->prototype, functionClass);
        fInst->fWrap = new FunctionWrapper(true, new ParameterFrame(JS2VAL_VOID, true), Object_toString, env);
        objectClass->prototype->writeProperty(this, engine->toString_StringAtom, OBJECT_TO_JS2VAL(fInst), 0);
        fInst->writeProperty(this, engine->length_StringAtom, INT_TO_JS2VAL(0), DynamicPropertyValue::READONLY);

/*** ECMA 3  Date Class ***/
        MAKEBUILTINCLASS(dateClass, objectClass, true, true, true, engine->allocStringPtr(&world.identifiers["Date"]), JS2VAL_NULL);
        v = new Variable(classClass, OBJECT_TO_JS2VAL(dateClass), true);
        defineLocalMember(env, &world.identifiers["Date"], &publicNamespaceList, Attribute::NoOverride, false, ReadWriteAccess, v, 0);
 //       dateClass->prototype = new PrototypeInstance(NULL, dateClass);
        initDateObject(this);

/*** ECMA 3  RegExp Class ***/
        MAKEBUILTINCLASS(regexpClass, objectClass, true, true, true, engine->allocStringPtr(&world.identifiers["RegExp"]), JS2VAL_NULL);
        v = new Variable(classClass, OBJECT_TO_JS2VAL(regexpClass), true);
        defineLocalMember(env, &world.identifiers["RegExp"], &publicNamespaceList, Attribute::NoOverride, false, ReadWriteAccess, v, 0);
        initRegExpObject(this);

/*** ECMA 3  String Class ***/
        v = new Variable(classClass, OBJECT_TO_JS2VAL(stringClass), true);
        defineLocalMember(env, &world.identifiers["String"], &publicNamespaceList, Attribute::NoOverride, false, ReadWriteAccess, v, 0);
        initStringObject(this);

/*** ECMA 3  Number Class ***/
        v = new Variable(classClass, OBJECT_TO_JS2VAL(numberClass), true);
        defineLocalMember(env, &world.identifiers["Number"], &publicNamespaceList, Attribute::NoOverride, false, ReadWriteAccess, v, 0);
        initNumberObject(this);

/*** ECMA 3  Boolean Class ***/
        v = new Variable(classClass, OBJECT_TO_JS2VAL(booleanClass), true);
        defineLocalMember(env, &world.identifiers["Boolean"], &publicNamespaceList, Attribute::NoOverride, false, ReadWriteAccess, v, 0);
        initBooleanObject(this);

/*** ECMA 3  Math Class ***/
        MAKEBUILTINCLASS(mathClass, objectClass, true, true, true, engine->allocStringPtr(&world.identifiers["Math"]), JS2VAL_NULL);
        v = new Variable(classClass, OBJECT_TO_JS2VAL(mathClass), true);
        defineLocalMember(env, &world.identifiers["Math"], &publicNamespaceList, Attribute::NoOverride, false, ReadWriteAccess, v, 0);
        initMathObject(this);

/*** ECMA 3  Array Class ***/
        MAKEBUILTINCLASS(arrayClass, objectClass, true, true, true, engine->allocStringPtr(&world.identifiers["Array"]), JS2VAL_NULL);
        v = new Variable(classClass, OBJECT_TO_JS2VAL(arrayClass), true);
        defineLocalMember(env, &world.identifiers["Array"], &publicNamespaceList, Attribute::NoOverride, false, ReadWriteAccess, v, 0);
        initArrayObject(this);

/*** ECMA 3  Error Classes ***/
        MAKEBUILTINCLASS(errorClass, objectClass, true, true, true, engine->allocStringPtr(&world.identifiers["Error"]), JS2VAL_NULL);
        v = new Variable(classClass, OBJECT_TO_JS2VAL(errorClass), true);
        defineLocalMember(env, &world.identifiers["Error"], &publicNamespaceList, Attribute::NoOverride, false, ReadWriteAccess, v, 0);
        MAKEBUILTINCLASS(evalErrorClass, objectClass, true, true, true, engine->allocStringPtr(&world.identifiers["EvalError"]), JS2VAL_NULL);
        v = new Variable(classClass, OBJECT_TO_JS2VAL(evalErrorClass), true);
        defineLocalMember(env, &world.identifiers["EvalError"], &publicNamespaceList, Attribute::NoOverride, false, ReadWriteAccess, v, 0);
        MAKEBUILTINCLASS(rangeErrorClass, objectClass, true, true, true, engine->allocStringPtr(&world.identifiers["RangeError"]), JS2VAL_NULL);
        v = new Variable(classClass, OBJECT_TO_JS2VAL(rangeErrorClass), true);
        defineLocalMember(env, &world.identifiers["RangeError"], &publicNamespaceList, Attribute::NoOverride, false, ReadWriteAccess, v, 0);
        MAKEBUILTINCLASS(referenceErrorClass, objectClass, true, true, true, engine->allocStringPtr(&world.identifiers["ReferenceError"]), JS2VAL_NULL);
        v = new Variable(classClass, OBJECT_TO_JS2VAL(referenceErrorClass), true);
        defineLocalMember(env, &world.identifiers["ReferenceError"], &publicNamespaceList, Attribute::NoOverride, false, ReadWriteAccess, v, 0);
        MAKEBUILTINCLASS(syntaxErrorClass, objectClass, true, true, true, engine->allocStringPtr(&world.identifiers["SyntaxError"]), JS2VAL_NULL);
        v = new Variable(classClass, OBJECT_TO_JS2VAL(syntaxErrorClass), true);
        defineLocalMember(env, &world.identifiers["SyntaxError"], &publicNamespaceList, Attribute::NoOverride, false, ReadWriteAccess, v, 0);
        MAKEBUILTINCLASS(typeErrorClass, objectClass, true, true, true, engine->allocStringPtr(&world.identifiers["TypeError"]), JS2VAL_NULL);
        v = new Variable(classClass, OBJECT_TO_JS2VAL(typeErrorClass), true);
        defineLocalMember(env, &world.identifiers["TypeError"], &publicNamespaceList, Attribute::NoOverride, false, ReadWriteAccess, v, 0);
        MAKEBUILTINCLASS(uriErrorClass, objectClass, true, true, true, engine->allocStringPtr(&world.identifiers["UriError"]), JS2VAL_NULL);
        v = new Variable(classClass, OBJECT_TO_JS2VAL(uriErrorClass), true);
        defineLocalMember(env, &world.identifiers["UriError"], &publicNamespaceList, Attribute::NoOverride, false, ReadWriteAccess, v, 0);
        initErrorObject(this);

    }

    // objectType(o) returns an OBJECT o's most specific type.
    JS2Class *JS2Metadata::objectType(js2val objVal)
    {
        if (JS2VAL_IS_VOID(objVal))
            return undefinedClass;
        if (JS2VAL_IS_NULL(objVal))
            return nullClass;
        if (JS2VAL_IS_BOOLEAN(objVal))
            return booleanClass;
        if (JS2VAL_IS_NUMBER(objVal))
            return numberClass;
        if (JS2VAL_IS_STRING(objVal)) {
            if (JS2VAL_TO_STRING(objVal)->length() == 1)
                return stringClass; // XXX characterClass; Need the connection from class Character to
                                    // class String - i.e. access to all the functions in 'String.prototype' - 
                                    // but (some of) those routines depened on being called on a StringInstance...
            else 
                return stringClass;
        }
        ASSERT(JS2VAL_IS_OBJECT(objVal));
        JS2Object *obj = JS2VAL_TO_OBJECT(objVal);
        switch (obj->kind) {
        case AttributeObjectKind:
            return attributeClass;
        case MultinameKind:
            return namespaceClass;
        case ClassKind:
            return classClass;
        case PrototypeInstanceKind: 
            return prototypeClass;

        case SimpleInstanceKind: 
            return checked_cast<SimpleInstance *>(obj)->type;

        case GlobalObjectKind: 
        case PackageKind:
            return packageClass;

        case MethodClosureKind:
            return functionClass;

        case SystemKind:
        case ParameterKind: 
        case BlockKind: 
        default:
            ASSERT(false);
            return NULL;
        }
    }


    // hasType(o, c) returns true if o is an instance of class c (or one of c's subclasses). 
    // It considers null to be an instance of the classes Null and Object only.
    bool JS2Metadata::hasType(js2val objVal, JS2Class *c)
    {
        return c->isAncestor(objectType(objVal));
    }

    // relaxedHasType(o, c) returns true if o is an instance of class c (or one of c's subclasses) 
    // but considers null to be an instance of the classes Null, Object, and all other non-primitive classes.
    bool JS2Metadata::relaxedHasType(js2val objVal, JS2Class *c)
    {
        JS2Class *t = objectType(objVal);
        return t->isAncestor(c) || (JS2VAL_IS_NULL(objVal) && t->allowNull);
    }


    // Scan this object and, if appropriate, it's prototype chain
    // looking for the given name. Return the containing object.
    JS2Object *JS2Metadata::lookupDynamicProperty(JS2Object *obj, const String *name)
    {
        ASSERT(obj);
        DynamicPropertyMap *dMap = NULL;
        bool isPrototypeInstance = false;
        if (obj->kind == SimpleInstanceKind)
            dMap = (checked_cast<SimpleInstance *>(obj))->dynamicProperties;
        else
        if (obj->kind == GlobalObjectKind)
            dMap = &(checked_cast<GlobalObject *>(obj))->dynamicProperties;
        else {
            ASSERT(obj->kind == PrototypeInstanceKind);
            isPrototypeInstance = true;
            dMap = &(checked_cast<PrototypeInstance *>(obj))->dynamicProperties;
        }
        DynamicPropertyBinding **dpbP = (*dMap)[*name];
        if (dpbP)
            return obj;
        if (isPrototypeInstance) {
            PrototypeInstance *pInst = checked_cast<PrototypeInstance *>(obj);
            if (pInst->parent)
                return lookupDynamicProperty(pInst->parent, name);
        }
        return NULL;
    }

    // Return true if the object contains the property, but don't
    // scan the prototype chain. Only scan dynamic properties. XXX
    bool JS2Metadata::hasOwnProperty(JS2Object *obj, const String *name)
    {
        ASSERT(obj);
        DynamicPropertyMap *dMap = NULL;
        bool isPrototypeInstance = false;
        if (obj->kind == SimpleInstanceKind)
            dMap = (checked_cast<SimpleInstance *>(obj))->dynamicProperties;
        else
        if (obj->kind == GlobalObjectKind)
            dMap = &(checked_cast<GlobalObject *>(obj))->dynamicProperties;
        else {
            ASSERT(obj->kind == PrototypeInstanceKind);
            isPrototypeInstance = true;
            dMap = &(checked_cast<PrototypeInstance *>(obj))->dynamicProperties;
        }
        DynamicPropertyBinding **dpbP = (*dMap)[*name];
        if (dpbP)
            return true;
        return false;
    }

    // Read the property from the container given by the public id in multiname - if that exists
    // 
    bool JS2Metadata::readDynamicProperty(JS2Object *container, Multiname *multiname, LookupKind *lookupKind, Phase phase, js2val *rval)
    {
        ASSERT(container && ((container->kind == SimpleInstanceKind) 
                                || (container->kind == GlobalObjectKind)
                                || (container->kind == PrototypeInstanceKind)));
        if (!multiname->listContains(publicNamespace))
            return false;
        const String *name = multiname->name;
        if (phase == CompilePhase) 
            reportError(Exception::compileExpressionError, "Inappropriate compile time expression", engine->errorPos());
        DynamicPropertyMap *dMap = NULL;
        bool isPrototypeInstance = false;
        if (container->kind == SimpleInstanceKind)
            dMap = (checked_cast<SimpleInstance *>(container))->dynamicProperties;
        else
        if (container->kind == GlobalObjectKind)
            dMap = &(checked_cast<GlobalObject *>(container))->dynamicProperties;
        else {
            isPrototypeInstance = true;
            dMap = &(checked_cast<PrototypeInstance *>(container))->dynamicProperties;
        }
        if (dMap == NULL)
            return false; // 'None'
        DynamicPropertyBinding **dpbP = (*dMap)[*name];
        if (dpbP) {
            *rval = (*dpbP)->v.value;
            return true;
        }
        if (isPrototypeInstance) {
            PrototypeInstance *pInst = checked_cast<PrototypeInstance *>(container);
            if (pInst->parent)
                return readDynamicProperty(pInst->parent, multiname, lookupKind, phase, rval);
        }
        if (lookupKind->isPropertyLookup()) {
            *rval = JS2VAL_UNDEFINED;
            return true;
        }
        return false;   // 'None'
    }

    void SimpleInstance::writeProperty(JS2Metadata * /* meta */, const String *name, js2val newValue, uint32 flags)
    {
        ASSERT(dynamicProperties);
        DynamicPropertyBinding *dpb = new DynamicPropertyBinding(*name, DynamicPropertyValue(newValue, flags));
        dynamicProperties->insert(dpb->name, dpb); 
    }

    void PrototypeInstance::writeProperty(JS2Metadata * /* meta */, const String *name, js2val newValue, uint32 flags)
    {
        DynamicPropertyBinding *dpb = new DynamicPropertyBinding(*name, DynamicPropertyValue(newValue, flags));
        dynamicProperties.insert(dpb->name, dpb); 
    }

    void ArrayInstance::writeProperty(JS2Metadata *meta, const String *name, js2val newValue, uint32 flags)
    {
        // An index has to pass the test that :
        //   ToString(ToUint32(ToString(index))) == ToString(index)     
        // (we already have done the 'ToString(index)' part, so require
        //
        //  ToString(ToUint32(name)) == name
        //

        // XXX things would go faster if the index made it here as an int
        // (which it more typically is) rather than converted to string
        // and back.

        DynamicPropertyBinding *dpb = new DynamicPropertyBinding(*name, DynamicPropertyValue(newValue, flags));
        dynamicProperties.insert(dpb->name, dpb); 

        const char16 *numEnd;        
        float64 f = stringToDouble(name->data(), name->data() + name->length(), numEnd);
        uint32 index = JS2Engine::float64toUInt32(f);

        char buf[dtosStandardBufferSize];
        const char *chrp = doubleToStr(buf, dtosStandardBufferSize, index, dtosStandard, 0);

        if (widenCString(chrp) == *name) {
            uint32 length = getLength(meta, this);
            if (index >= length)
                setLength(meta, this, index + 1);
        }
    }

    // Write a value to a dynamic container - inserting into the map if not already there (if createIfMissing)
    bool JS2Metadata::writeDynamicProperty(JS2Object *container, Multiname *multiname, bool createIfMissing, js2val newValue, Phase phase)
    {
        ASSERT(container && ((container->kind == SimpleInstanceKind) 
                                || (container->kind == GlobalObjectKind)
                                || (container->kind == PrototypeInstanceKind)));
        if (!multiname->listContains(publicNamespace))
            return false;
        const String *name = multiname->name;
        DynamicPropertyMap *dMap = NULL;
        if (container->kind == SimpleInstanceKind)
            dMap = (checked_cast<SimpleInstance *>(container))->dynamicProperties;
        else
        if (container->kind == GlobalObjectKind)
            dMap = &(checked_cast<GlobalObject *>(container))->dynamicProperties;
        else 
            dMap = &(checked_cast<PrototypeInstance *>(container))->dynamicProperties;
        if (dMap == NULL)
            return false; // 'None'
        DynamicPropertyBinding **dpbP = (*dMap)[*name];
        if (dpbP) {
            // special case handling for setting 'length' property of ArrayInstances
            // XXX should handle this with dispatch to 'writeProperty' of each of
            // the different dynamic map containing objects, and change current
            // 'writeProperty' functions to 'addProperty'.
            if ((container->kind == PrototypeInstanceKind)
                    && ((checked_cast<PrototypeInstance *>(container))->type == arrayClass)
                    && (*name == *engine->length_StringAtom)) {
                float64 f = toFloat64(newValue);
                uint32 newLength = (uint32)f;
                if (f != newLength)
                    reportError(Exception::rangeError, "Invalid length value", engine->errorPos());
                setLength(this, container, newLength);
            }
            else
                if (((*dpbP)->v.flags & DynamicPropertyValue::READONLY) == 0)
                    (*dpbP)->v.value = newValue;
            return true;
        }
        if (!createIfMissing)
            return false;
        if (container->kind == SimpleInstanceKind) {
            SimpleInstance *dynInst = checked_cast<SimpleInstance *>(container);
            QualifiedName qname;
            InstanceBinding *ib = resolveInstanceMemberName(dynInst->type, multiname, ReadAccess, phase, &qname);
            if (ib == NULL) {
                dynInst->writeProperty(this, name, newValue, 0);
                return true;
            }
        }
        else {
            if (container->kind == GlobalObjectKind) {
                GlobalObject *glob = checked_cast<GlobalObject *>(container);
                LocalMember *m = findFlatMember(glob, multiname, ReadAccess, phase);
                if (m == NULL) {
                    DynamicPropertyBinding *dpb = new DynamicPropertyBinding(*name, DynamicPropertyValue(newValue, DynamicPropertyValue::ENUMERATE));
                    glob->dynamicProperties.insert(dpb->name, dpb); 
                    return true;
                }
            }
            else {
                PrototypeInstance *pInst = checked_cast<PrototypeInstance *>(container);
                pInst->writeProperty(this, name, newValue, 0);
                return true;
            }
        }
        return false;   // 'None'
    }

    // Read a value from the local member
    bool JS2Metadata::readLocalMember(LocalMember *m, Phase phase, js2val *rval)
    {
        if (m == NULL)
            return false;   // 'None'
        switch (m->kind) {
        case LocalMember::Forbidden:
            reportError(Exception::propertyAccessError, "Forbidden access", engine->errorPos());
            break;
        case LocalMember::Variable:
            {
                Variable *v = checked_cast<Variable *>(m); 
                if ((phase == CompilePhase) && !v->immutable)
                    reportError(Exception::compileExpressionError, "Inappropriate compile time expression", engine->errorPos());
                *rval = v->value;
                return true;
            }
        case LocalMember::DynamicVariableKind:
            if (phase == CompilePhase) 
                reportError(Exception::compileExpressionError, "Inappropriate compile time expression", engine->errorPos());
            *rval = (checked_cast<DynamicVariable *>(m))->value;
            return true;
        case LocalMember::ConstructorMethod:
            {
                if (phase == CompilePhase)
                    reportError(Exception::compileExpressionError, "Inappropriate compile time expression", engine->errorPos());
                *rval = (checked_cast<ConstructorMethod *>(m))->value;
                return true;
            }
        case LocalMember::Getter:
        case LocalMember::Setter:
            break;
        }
        NOT_REACHED("Bad member kind");
        return false;
    }

    // Write a value to the local member
    bool JS2Metadata::writeLocalMember(LocalMember *m, js2val newValue, Phase /* phase */, bool initFlag) // phase not used?
    {
        if (m == NULL)
            return false;   // 'None'
        switch (m->kind) {
        case LocalMember::Forbidden:
        case LocalMember::ConstructorMethod:
            reportError(Exception::propertyAccessError, "Forbidden access", engine->errorPos());
            break;
        case LocalMember::Variable:
            {
                Variable *v = checked_cast<Variable *>(m);
                if (!initFlag
                        && (JS2VAL_IS_INACCESSIBLE(v->value) 
                                || (v->immutable && !JS2VAL_IS_UNINITIALIZED(v->value))))
                    if (flags == JS2)
                        reportError(Exception::propertyAccessError, "Forbidden access", engine->errorPos());
                    else    // quietly ignore the write for JS1 compatibility
                        return true;
                v->value = v->type->implicitCoerce(this, newValue);
            }
            return true;
        case LocalMember::DynamicVariableKind:
            (checked_cast<DynamicVariable *>(m))->value = newValue;
            return true;
        case LocalMember::Getter:
        case LocalMember::Setter:
            break;
        }
        NOT_REACHED("Bad member kind");
        return false;
    }

    // Read the value of a property in the container. Return true/false if that container has
    // the property or not. If it does, return it's value
    bool JS2Metadata::readProperty(js2val *containerVal, Multiname *multiname, LookupKind *lookupKind, Phase phase, js2val *rval)
    {
        bool isSimpleInstance = false;
        JS2Object *container;
        if (JS2VAL_IS_PRIMITIVE(*containerVal)) {
readClassProperty:
            JS2Class *c = objectType(*containerVal);
            QualifiedName qname;
            InstanceBinding *ib = resolveInstanceMemberName(c, multiname, ReadAccess, phase, &qname);
            if ((ib == NULL) && isSimpleInstance) 
                return readDynamicProperty(JS2VAL_TO_OBJECT(*containerVal), multiname, lookupKind, phase, rval);
            else {
                // XXX Spec. would have us passing a primitive here since ES4 is 'not addressing' the issue
                // of so-called wrapper objects.
                if (!JS2VAL_IS_OBJECT(*containerVal))
                    *containerVal = toObject(*containerVal);
                else
                    return readInstanceMember(*containerVal, c, (ib) ? &qname : NULL, phase, rval);
            }
        }
        container = JS2VAL_TO_OBJECT(*containerVal);
        switch (container->kind) {
        case AttributeObjectKind:
        case MultinameKind:
        case MethodClosureKind:
            goto readClassProperty;
        case SimpleInstanceKind:
            isSimpleInstance = true;
            goto readClassProperty;

        case SystemKind:
        case GlobalObjectKind: 
        case PackageKind:
        case ParameterKind: 
        case BlockKind: 
        case ClassKind:
            return readProperty(checked_cast<Frame *>(container), multiname, lookupKind, phase, rval);

        case PrototypeInstanceKind: 
            return readDynamicProperty(container, multiname, lookupKind, phase, rval);

        case AlienInstanceKind:
            return (checked_cast<AlienInstance *>(container))->readProperty(multiname, rval);

        default:
            ASSERT(false);
            return false;
        }
    }

    // Use the slotIndex from the instanceVariable to access the slot
    Slot *JS2Metadata::findSlot(js2val thisObjVal, InstanceVariable *id)
    {
        ASSERT(JS2VAL_IS_OBJECT(thisObjVal) 
                    && (JS2VAL_TO_OBJECT(thisObjVal)->kind == SimpleInstanceKind));
        JS2Object *thisObj = JS2VAL_TO_OBJECT(thisObjVal);
        return &checked_cast<SimpleInstance *>(thisObj)->slots[id->slotIndex];
    }

    // Read the value of an instanceMember, if valid
    bool JS2Metadata::readInstanceMember(js2val containerVal, JS2Class *c, QualifiedName *qname, Phase phase, js2val *rval)
    {
        InstanceMember *m = findInstanceMember(c, qname, ReadAccess);
        if (m == NULL) return false;
        switch (m->kind) {
        case InstanceMember::InstanceVariableKind:
            {
                InstanceVariable *mv = checked_cast<InstanceVariable *>(m);
                if ((phase == CompilePhase) && !mv->immutable)
                    reportError(Exception::compileExpressionError, "Inappropriate compile time expression", engine->errorPos());
                Slot *s = findSlot(containerVal, mv);
                if (JS2VAL_IS_UNINITIALIZED(s->value))
                    reportError(Exception::uninitializedError, "Reference to uninitialized instance variable", engine->errorPos());
                *rval = s->value;
                return true;
            }
            break;

        case InstanceMember::InstanceMethodKind:
            {
                *rval = OBJECT_TO_JS2VAL(new MethodClosure(containerVal, checked_cast<InstanceMethod *>(m)));
                return true;
            }
            break;

        case InstanceMember::InstanceAccessorKind:
            break;
        }
        ASSERT(false);
        return false;
    }

    // Read the value of a property in the frame. Return true/false if that frame has
    // the property or not. If it does, return it's value
    bool JS2Metadata::readProperty(Frame *container, Multiname *multiname, LookupKind *lookupKind, Phase phase, js2val *rval)
    {
        if (container->kind != ClassKind) {
            if (container->kind == WithFrameKind) {
                return readDynamicProperty(checked_cast<WithFrame *>(container)->obj, multiname, lookupKind, phase, rval);
            }
            else {
                // Must be System, Global, Package, Parameter or Block
                LocalMember *m = findFlatMember(checked_cast<NonWithFrame *>(container), multiname, ReadAccess, phase);
                if (!m && (container->kind == GlobalObjectKind))
                    return readDynamicProperty(container, multiname, lookupKind, phase, rval);
                else
                    return readLocalMember(m, phase, rval);
            }
        }
        else {
            // XXX using JS2VAL_UNINITIALIZED to signal generic 'this'
            js2val thisObject;
            if (lookupKind->isPropertyLookup()) 
                thisObject = JS2VAL_UNINITIALIZED;
            else
                thisObject = lookupKind->thisObject;
            MemberDescriptor m2;
            if (!findLocalMember(checked_cast<JS2Class *>(container), multiname, ReadAccess, phase, &m2) 
                        || m2.localMember)
                return readLocalMember(m2.localMember, phase, rval);
            else {
                if (JS2VAL_IS_NULL(thisObject))
                    reportError(Exception::propertyAccessError, "Null 'this' object", engine->errorPos());
                if (JS2VAL_IS_INACCESSIBLE(thisObject))
                    reportError(Exception::compileExpressionError, "Inaccesible 'this' object", engine->errorPos());
                if (JS2VAL_IS_UNINITIALIZED(thisObject)) {
                    // 'this' is {generic}
                    // XXX is ??? in spec.
                }
                QualifiedName qname(m2.ns, multiname->name);
                return readInstanceMember(thisObject, objectType(thisObject), &qname, phase, rval);
            }
        }
    }

    // Write the value of a property in the container. Return true/false if that container has
    // the property or not.
    bool JS2Metadata::writeProperty(js2val containerVal, Multiname *multiname, LookupKind *lookupKind, bool createIfMissing, js2val newValue, Phase phase)
    {
        if (JS2VAL_IS_PRIMITIVE(containerVal))
            return false;
        JS2Object *container = JS2VAL_TO_OBJECT(containerVal);
        switch (container->kind) {
        case AttributeObjectKind:
        case MultinameKind:
        case MethodClosureKind:
            return false;

        case SimpleInstanceKind:
            {
                JS2Class *c = checked_cast<SimpleInstance *>(container)->type;
                QualifiedName qname;
                InstanceBinding *ib = resolveInstanceMemberName(c, multiname, WriteAccess, phase, &qname);
                if (ib == NULL)
                    return writeDynamicProperty(container, multiname, createIfMissing, newValue, phase);
                else
                    return writeInstanceMember(containerVal, c, (ib) ? &qname : NULL, newValue, phase); 
            }

        case SystemKind:
        case GlobalObjectKind: 
        case PackageKind:
        case ParameterKind: 
        case BlockKind: 
        case ClassKind:
            return writeProperty(checked_cast<Frame *>(container), multiname, lookupKind, createIfMissing, newValue, phase, false);

        case PrototypeInstanceKind: 
            return writeDynamicProperty(container, multiname, createIfMissing, newValue, phase);

        default:
            ASSERT(false);
            return false;
        }

    }
    
    // Write the value of an instance member into a container instance.
    // Only instanceVariables are writable.
    bool JS2Metadata::writeInstanceMember(js2val containerVal, JS2Class *c, QualifiedName *qname, js2val newValue, Phase phase)
    {
        if (phase == CompilePhase)
            reportError(Exception::compileExpressionError, "Inappropriate compile time expression", engine->errorPos());
        InstanceMember *m = findInstanceMember(c, qname, WriteAccess);
        if (m == NULL) return false;
        switch (m->kind) {
        case InstanceMember::InstanceVariableKind:
            {
                InstanceVariable *mv = checked_cast<InstanceVariable *>(m);
                Slot *s = findSlot(containerVal, mv);
                if (mv->immutable && JS2VAL_IS_INITIALIZED(s->value))
                    reportError(Exception::propertyAccessError, "Reinitialization of constant", engine->errorPos());
                s->value = mv->type->implicitCoerce(this, newValue);
                return true;
            }
        
        case InstanceMember::InstanceMethodKind:
        case InstanceMember::InstanceAccessorKind:
            reportError(Exception::propertyAccessError, "Attempt to write to a method", engine->errorPos());
            break;
        }
        ASSERT(false);
        return false;

    }

    // Write the value of a property in the frame. Return true/false if that frame has
    // the property or not.
    bool JS2Metadata::writeProperty(Frame *container, Multiname *multiname, LookupKind *lookupKind, bool createIfMissing, js2val newValue, Phase phase, bool initFlag)
    {
        if (container->kind != ClassKind) {
            if (container->kind == WithFrameKind) {
                return writeDynamicProperty(checked_cast<WithFrame *>(container)->obj, multiname, createIfMissing, newValue, phase);
            }
            else {
                // Must be System, Global, Package, Parameter or Block
                LocalMember *m = findFlatMember(checked_cast<NonWithFrame *>(container), multiname, WriteAccess, phase);
                if (!m && (container->kind == GlobalObjectKind))
                    return writeDynamicProperty(container, multiname, createIfMissing, newValue, phase);
                else
                    return writeLocalMember(m, newValue, phase, initFlag);
            }
        }
        else {
            // XXX using JS2VAL_UNINITIALIZED to signal generic 'this'
            js2val thisObject;
            if (lookupKind->isPropertyLookup()) 
                thisObject = JS2VAL_UNINITIALIZED;
            else
                thisObject = lookupKind->thisObject;
            MemberDescriptor m2;
            if (!findLocalMember(checked_cast<JS2Class *>(container), multiname, WriteAccess, phase, &m2) 
                            || m2.localMember)
                return writeLocalMember(m2.localMember, newValue, phase, initFlag);
            else {
                if (JS2VAL_IS_NULL(thisObject))
                    reportError(Exception::propertyAccessError, "Null 'this' object", engine->errorPos());
                if (JS2VAL_IS_VOID(thisObject))
                    reportError(Exception::compileExpressionError, "Undefined 'this' object", engine->errorPos());
                if (JS2VAL_IS_UNINITIALIZED(thisObject)) {
                    // 'this' is {generic}
                    // XXX is ??? in spec.
                }
                QualifiedName qname(m2.ns, multiname->name);
                return writeInstanceMember(thisObject, objectType(thisObject), &qname, newValue, phase);
            }
        }
    }

    bool JS2Metadata::deleteProperty(js2val containerVal, Multiname *multiname, LookupKind *lookupKind, Phase phase, bool *result)
    {
        ASSERT(phase == RunPhase);
        bool isSimpleInstance = false;
        if (JS2VAL_IS_PRIMITIVE(containerVal)) {
deleteClassProperty:
            JS2Class *c = objectType(containerVal);
            QualifiedName qname;
            InstanceBinding *ib = resolveInstanceMemberName(c, multiname, ReadAccess, phase, &qname);
            if ((ib == NULL) && isSimpleInstance) 
                return deleteDynamicProperty(JS2VAL_TO_OBJECT(containerVal), multiname, lookupKind, result);
            else 
                return deleteInstanceMember(c, (ib) ? &qname : NULL, result);
        }
        JS2Object *container = JS2VAL_TO_OBJECT(containerVal);
        switch (container->kind) {
        case AttributeObjectKind:
        case MultinameKind:
        case MethodClosureKind:
            goto deleteClassProperty;
        case SimpleInstanceKind:
            isSimpleInstance = true;
            goto deleteClassProperty;

        case SystemKind:
        case GlobalObjectKind: 
        case PackageKind:
        case ParameterKind: 
        case BlockKind: 
        case ClassKind:
            return deleteProperty(checked_cast<Frame *>(container), multiname, lookupKind, phase, result);

        case PrototypeInstanceKind: 
            return deleteDynamicProperty(container, multiname, lookupKind, result);

        default:
            ASSERT(false);
            return false;
        }
    }

    bool JS2Metadata::deleteProperty(Frame *container, Multiname *multiname, LookupKind *lookupKind, Phase phase, bool *result)
    {
        ASSERT(phase == RunPhase);
        if (container->kind != ClassKind) {
            if (container->kind == WithFrameKind) {                
                return deleteDynamicProperty(checked_cast<WithFrame *>(container)->obj, multiname, lookupKind, result);
            }
            else {
                // Must be System, Global, Package, Parameter or Block
                LocalMember *m = findFlatMember(checked_cast<NonWithFrame *>(container), multiname, ReadAccess, phase);
                if (!m && (container->kind == GlobalObjectKind))
                    return deleteDynamicProperty(container, multiname, lookupKind, result);
                else
                    return deleteLocalMember(m, result);
            }
        }
        else {
            // XXX using JS2VAL_UNINITIALIZED to signal generic 'this'
            js2val thisObject;
            if (lookupKind->isPropertyLookup()) 
                thisObject = JS2VAL_UNINITIALIZED;
            else
                thisObject = lookupKind->thisObject;
            MemberDescriptor m2;
            if (findLocalMember(checked_cast<JS2Class *>(container), multiname, ReadAccess, phase, &m2) && m2.localMember)
                return deleteLocalMember(m2.localMember, result);
            else {
                if (JS2VAL_IS_NULL(thisObject))
                    reportError(Exception::propertyAccessError, "Null 'this' object", engine->errorPos());
                if (JS2VAL_IS_UNINITIALIZED(thisObject)) {
                    *result = false;
                    return true;
                }
                QualifiedName qname(m2.ns, multiname->name);
                return deleteInstanceMember(objectType(thisObject), &qname, result);
            }
        }
    }

    bool JS2Metadata::deleteDynamicProperty(JS2Object *container, Multiname *multiname, LookupKind * /* lookupKind */, bool *result)
    {
        ASSERT(container && ((container->kind == SimpleInstanceKind) 
                                || (container->kind == GlobalObjectKind)
                                || (container->kind == PrototypeInstanceKind)));
        if (!multiname->listContains(publicNamespace))
            return false;
        const String *name = multiname->name;
        DynamicPropertyMap *dMap = NULL;
        if (container->kind == SimpleInstanceKind)
            dMap = (checked_cast<SimpleInstance *>(container))->dynamicProperties;
        else
        if (container->kind == GlobalObjectKind)
            dMap = &(checked_cast<GlobalObject *>(container))->dynamicProperties;
        else {
            dMap = &(checked_cast<PrototypeInstance *>(container))->dynamicProperties;
        }
        DynamicPropertyBinding **dpbP = (*dMap)[*name];
        if (dpbP) {
            if (((*dpbP)->v.flags & DynamicPropertyValue::PERMANENT) == 0) {
                dMap->erase(*name);         // XXX more efficient scheme?
                *result = true;
                return true;
            }
        }
        return false;
    }

    bool JS2Metadata::deleteLocalMember(LocalMember *m, bool *result)
    {
        if (m == NULL)
            return false;   // 'None'
        switch (m->kind) {
        case LocalMember::Forbidden:
            reportError(Exception::propertyAccessError, "Forbidden access", engine->errorPos());
            break;
        case LocalMember::Variable:
        case LocalMember::DynamicVariableKind:
        case LocalMember::ConstructorMethod:
        case LocalMember::Getter:
        case LocalMember::Setter:
            *result = false;
            return true;
        }
        NOT_REACHED("Bad member kind");
        return false;
    }

    bool JS2Metadata::deleteInstanceMember(JS2Class *c, QualifiedName *qname, bool *result)
    {
        InstanceMember *m = findInstanceMember(c, qname, ReadAccess);
        if (m == NULL) return false;
        *result = false;
        return true;
    }


    // Find a binding that matches the multiname and access.
    // It's an error if more than one such binding exists.
    LocalMember *JS2Metadata::findFlatMember(NonWithFrame *container, Multiname *multiname, Access access, Phase /* phase */)
    {
        LocalMember *found = NULL;
        
        LocalBindingEntry **lbeP = container->localBindings[*multiname->name];
        if (lbeP) {
            for (LocalBindingEntry::NS_Iterator i = (*lbeP)->begin(), end = (*lbeP)->end(); (i != end); i++) {
                LocalBindingEntry::NamespaceBinding &ns = *i;
                if ((ns.second->accesses & access) && multiname->listContains(ns.first)) {
                    if (found && (ns.second->content != found))
                        reportError(Exception::propertyAccessError, "Ambiguous reference to {0}", engine->errorPos(), multiname->name);
                    else
                        found = ns.second->content;
                }
            }
        }
        return found;
    }

    // Find the multiname in the class - either in the local bindings (first) or
    // in the instance bindings. If not there, look in the super class.
    bool JS2Metadata::findLocalMember(JS2Class *c, Multiname *multiname, Access access, Phase /* phase */, MemberDescriptor *result)
    {
        result->localMember = NULL;
        result->ns = NULL;
        JS2Class *s = c;
        while (s) {
            LocalMember *found = NULL;
            LocalBindingEntry **lbeP = s->localBindings[*multiname->name];
            if (lbeP) {
                for (LocalBindingEntry::NS_Iterator i = (*lbeP)->begin(), end = (*lbeP)->end(); (i != end); i++) {
                    LocalBindingEntry::NamespaceBinding &ns = *i;
                    if ((ns.second->accesses & access) && multiname->listContains(ns.first)) {
                        if (found && (ns.second->content != found))
                            reportError(Exception::propertyAccessError, "Ambiguous reference to {0}", engine->errorPos(), multiname->name);
                        else {
                            found = ns.second->content;
                            result->localMember = found;
                        }
                    }
                }
            }
            if (found)
                return true;

            InstanceBinding *iFound = NULL;
            InstanceBindingEntry **ibeP = s->instanceBindings[*multiname->name];
            if (ibeP) {
                for (InstanceBindingEntry::NS_Iterator i = (*ibeP)->begin(), end = (*ibeP)->end(); (i != end); i++) {
                    InstanceBindingEntry::NamespaceBinding &ns = *i;
                    if ((ns.second->accesses & access) && multiname->listContains(ns.first))
                        if (iFound && (ns.second->content != iFound->content))
                            reportError(Exception::propertyAccessError, "Ambiguous reference to {0}", engine->errorPos(), multiname->name);
                        else {
                            iFound = ns.second;
                            result->ns = ns.first;
                        }
                    }
            }
            
            if (iFound)
                return true;
            s = s->super;
        }
        return false;
    }

    /*
    * Start from the root class (Object) and proceed through more specific classes that are ancestors of c.
    * Find the binding that matches the given access and multiname, it's an error if more than one such exists.
    *
    */
    InstanceBinding *JS2Metadata::resolveInstanceMemberName(JS2Class *c, Multiname *multiname, Access access, Phase phase, QualifiedName *qname)
    {
        InstanceBinding *result = NULL;
        if (c->super) {
            result = resolveInstanceMemberName(c->super, multiname, access, phase, qname);
            if (result) return result;
        }
        InstanceBindingEntry **ibeP = c->instanceBindings[*multiname->name];
        if (ibeP) {
            for (InstanceBindingEntry::NS_Iterator i = (*ibeP)->begin(), end = (*ibeP)->end(); (i != end); i++) {
                InstanceBindingEntry::NamespaceBinding &ns = *i;
                if ((ns.second->accesses & access) && multiname->listContains(ns.first))
                    if (result && (ns.second->content != result->content))
                        reportError(Exception::propertyAccessError, "Ambiguous reference to {0}", engine->errorPos(), multiname->name);
                    else {
                        result = ns.second;
                        qname->id = multiname->name;
                        qname->nameSpace = ns.first;
                    }
            }
        }
        return result;
    }

    // gc-mark all contained JS2Objects and their children 
    // and then invoke mark on all other structures that contain JS2Objects
    void JS2Metadata::markChildren()
    {        
        // XXX - maybe have a separate pool to allocate chunks
        // that are meant to be never collected?
        GCMARKOBJECT(publicNamespace);
        forbiddenMember->mark();

        GCMARKOBJECT(objectClass);
        GCMARKOBJECT(undefinedClass);
        GCMARKOBJECT(nullClass);
        GCMARKOBJECT(booleanClass);
        GCMARKOBJECT(generalNumberClass);
        GCMARKOBJECT(numberClass);
        GCMARKOBJECT(characterClass);
        GCMARKOBJECT(stringClass);
        GCMARKOBJECT(namespaceClass);
        GCMARKOBJECT(attributeClass);
        GCMARKOBJECT(classClass);
        GCMARKOBJECT(functionClass);
        GCMARKOBJECT(prototypeClass);
        GCMARKOBJECT(packageClass);
        GCMARKOBJECT(dateClass);
        GCMARKOBJECT(regexpClass);
        GCMARKOBJECT(mathClass);
        GCMARKOBJECT(arrayClass);
        GCMARKOBJECT(errorClass);
        GCMARKOBJECT(evalErrorClass);
        GCMARKOBJECT(rangeErrorClass);
        GCMARKOBJECT(referenceErrorClass);
        GCMARKOBJECT(syntaxErrorClass);
        GCMARKOBJECT(typeErrorClass);
        GCMARKOBJECT(uriErrorClass);

        for (BConListIterator i = bConList.begin(), end = bConList.end(); (i != end); i++)
            (*i)->mark();
        if (bCon)
            bCon->mark();
        if (engine)
            engine->mark();
        GCMARKOBJECT(env);

        GCMARKOBJECT(glob);
    
    }

    /*
     * Throw an exception of the specified kind, indicating the position 'pos' and
     * attaching the given message. If 'arg' is specified, replace {0} in the message
     * with the argument value. [This is intended to be widened into a more complete
     * argument handling scheme].
     */
    void JS2Metadata::reportError(Exception::Kind kind, const char *message, size_t pos, const char *arg)
    {
        const char16 *lineBegin;
        const char16 *lineEnd;
        String x = widenCString(message);
        if (arg) {
            // XXX handle multiple occurences and extend to {1} etc.
            uint32 a = x.find(widenCString("{0}"));
            x.replace(a, 3, widenCString(arg));
        }
        Reader reader(engine->bCon->mSource, engine->bCon->mSourceLocation, 1);
        reader.fillLineStartsTable();
        uint32 lineNum = reader.posToLineNum(pos);
        size_t linePos = reader.getLine(lineNum, lineBegin, lineEnd);
        ASSERT(lineBegin && lineEnd && linePos <= pos);
        throw Exception(kind, x, reader.sourceLocation, 
                            lineNum, pos - linePos, pos, lineBegin, lineEnd);
    }

    // Accepts a String as the error argument and converts to char *
    void JS2Metadata::reportError(Exception::Kind kind, const char *message, size_t pos, const String &name)
    {
        std::string str(name.length(), char());
        std::transform(name.begin(), name.end(), str.begin(), narrow);
        reportError(kind, message, pos, str.c_str());
    }

    // Accepts a String * as the error argument and converts to char *
    void JS2Metadata::reportError(Exception::Kind kind, const char *message, size_t pos, const String *name)
    {
        std::string str(name->length(), char());
        std::transform(name->begin(), name->end(), str.begin(), narrow);
        reportError(kind, message, pos, str.c_str());
    }

 
    void JS2Metadata::initBuiltinClass(JS2Class *builtinClass, FunctionData *protoFunctions, FunctionData *staticFunctions, NativeCode *construct, NativeCode *call)
    {
        FunctionData *pf;

        builtinClass->construct = construct;
        builtinClass->call = call;

        NamespaceList publicNamespaceList;
        publicNamespaceList.push_back(publicNamespace);
    
        // Adding "prototype" & "length", etc as static members of the class - not dynamic properties; XXX
        env->addFrame(builtinClass);
        {
            Variable *v = new Variable(builtinClass, OBJECT_TO_JS2VAL(builtinClass->prototype), true);
            defineLocalMember(env, engine->prototype_StringAtom, &publicNamespaceList, Attribute::NoOverride, false, ReadWriteAccess, v, 0);
            v = new Variable(builtinClass, INT_TO_JS2VAL(1), true);
            defineLocalMember(env, engine->length_StringAtom, &publicNamespaceList, Attribute::NoOverride, false, ReadWriteAccess, v, 0);

            pf = staticFunctions;
            if (pf) {
                while (pf->name) {
                    SimpleInstance *callInst = new SimpleInstance(functionClass);
                    callInst->fWrap = new FunctionWrapper(true, new ParameterFrame(JS2VAL_INACCESSIBLE, true), pf->code, env);
                    v = new Variable(functionClass, OBJECT_TO_JS2VAL(callInst), true);
                    defineLocalMember(env, &world.identifiers[pf->name], &publicNamespaceList, Attribute::NoOverride, false, ReadWriteAccess, v, 0);
                    writeDynamicProperty(callInst, new Multiname(engine->length_StringAtom, publicNamespace), true, INT_TO_JS2VAL(pf->length), RunPhase);
                    pf++;
                }
            }
        }
        env->removeTopFrame();
    
        // Add "constructor" as a dynamic property of the prototype
        FunctionInstance *fInst = new FunctionInstance(this, functionClass->prototype, functionClass);
        writeDynamicProperty(fInst, new Multiname(engine->length_StringAtom, publicNamespace), true, INT_TO_JS2VAL(1), RunPhase);
        fInst->fWrap = new FunctionWrapper(true, new ParameterFrame(JS2VAL_INACCESSIBLE, true), construct, env);
        writeDynamicProperty(builtinClass->prototype, new Multiname(&world.identifiers["constructor"], publicNamespace), true, OBJECT_TO_JS2VAL(fInst), RunPhase);
    
        pf = protoFunctions;
        if (pf) {
            while (pf->name) {
                SimpleInstance *callInst = new SimpleInstance(functionClass);
                callInst->fWrap = new FunctionWrapper(true, new ParameterFrame(JS2VAL_INACCESSIBLE, true), pf->code, env);
    /*
    XXX not prototype object function properties, like ECMA3
            writeDynamicProperty(dateClass->prototype, new Multiname(world.identifiers[pf->name], publicNamespace), true, OBJECT_TO_JS2VAL(fInst), RunPhase);
    */
    /*
    XXX not static members, since those can't be accessed from the instance
              Variable *v = new Variable(functionClass, OBJECT_TO_JS2VAL(fInst), true);
              defineLocalMember(&env, &world.identifiers[pf->name], &publicNamespaceList, Attribute::NoOverride, false, ReadWriteAccess, v, 0);
    */
                InstanceMember *m = new InstanceMethod(callInst);
                defineInstanceMember(builtinClass, &cxt, &world.identifiers[pf->name], &publicNamespaceList, Attribute::NoOverride, false, ReadWriteAccess, m, 0);

                FunctionInstance *fInst = new FunctionInstance(this, functionClass->prototype, functionClass);
                fInst->fWrap = callInst->fWrap;
                writeDynamicProperty(builtinClass->prototype, new Multiname(&world.identifiers[pf->name], publicNamespace), true, OBJECT_TO_JS2VAL(fInst), RunPhase);
                writeDynamicProperty(fInst, new Multiname(engine->length_StringAtom, publicNamespace), true, INT_TO_JS2VAL(pf->length), RunPhase);
                pf++;
            }
        }
    }

   
    
 /************************************************************************************
 *
 *  JS2Class
 *
 ************************************************************************************/


    JS2Class::JS2Class(JS2Class *super, JS2Object *proto, Namespace *privateNamespace, bool dynamic, bool allowNull, bool final, const String *name) 
        : NonWithFrame(ClassKind), 
            instanceInitOrder(NULL), 
            complete(false), 
            super(super), 
            prototype(proto), 
            privateNamespace(privateNamespace), 
            dynamic(dynamic),
            allowNull(allowNull),
            final(final),
            call(NULL),
            construct(JS2Engine::defaultConstructor),
            defaultValue(JS2VAL_NULL),
            slotCount(super ? super->slotCount : 0),
            name(name)
    {

    }

    // gc-mark all contained JS2Objects and visit contained structures to do likewise
    void JS2Class::markChildren()
    {
        NonWithFrame::markChildren();
        GCMARKOBJECT(super)
        GCMARKOBJECT(prototype)
        GCMARKOBJECT(privateNamespace)
        if (name) JS2Object::mark(name);
        GCMARKVALUE(defaultValue);
        for (InstanceBindingIterator rib = instanceBindings.begin(), riend = instanceBindings.end(); (rib != riend); rib++) {
            InstanceBindingEntry *ibe = *rib;
            for (InstanceBindingEntry::NS_Iterator i = ibe->begin(), end = ibe->end(); (i != end); i++) {
                InstanceBindingEntry::NamespaceBinding ns = *i;
                ns.second->content->mark();
            }
        }
    }

    // return true if 'heir' is this class or is any antecedent
    bool JS2Class::isAncestor(JS2Class *heir)
    {
        JS2Class *kinsman = this;
        do {
            if (heir == kinsman)
                return true;
            kinsman = kinsman->super;
        } while (kinsman);
        return false;
    }

    js2val JS2Class::implicitCoerce(JS2Metadata *meta, js2val newValue)
    {
        if (meta->relaxedHasType(newValue, this))
            return newValue;
        meta->reportError(Exception::badValueError, "Illegal coercion", meta->engine->errorPos());
        return JS2VAL_VOID;
    }

    void JS2Class::emitDefaultValue(BytecodeContainer *bCon, size_t pos)
    {
        if (JS2VAL_IS_NULL(defaultValue))
            bCon->emitOp(eNull, pos);
        else 
        if (JS2VAL_IS_VOID(defaultValue))
            bCon->emitOp(eUndefined, pos);
        else 
        if (JS2VAL_IS_BOOLEAN(defaultValue) && !JS2VAL_TO_BOOLEAN(defaultValue))
            bCon->emitOp(eFalse, pos);
        else
        if ((JS2VAL_IS_LONG(defaultValue) || JS2VAL_IS_ULONG(defaultValue)) 
                && (*JS2VAL_TO_LONG(defaultValue) == 0))
            bCon->emitOp(eLongZero, pos);
        else
            NOT_REACHED("unrecognized default value");
    }


/************************************************************************************
 *
 *  LocalBindingEntry
 *
 ************************************************************************************/

    // Clear all the clone contents for this entry
    void LocalBindingEntry::clear()
    {
        for (NS_Iterator i = bindingList.begin(), end = bindingList.end(); (i != end); i++) {
            NamespaceBinding &ns = *i;
            ns.second->content->cloneContent = NULL;
        }
    }

    // This version of 'clone' is used to construct a duplicate LocalBinding entry 
    // with each LocalBinding content copied from the (perhaps previously) cloned member.
    // See 'instantiateFrame'.
    LocalBindingEntry *LocalBindingEntry::clone()
    {
        LocalBindingEntry *new_e = new LocalBindingEntry(name);
        for (NS_Iterator i = bindingList.begin(), end = bindingList.end(); (i != end); i++) {
            NamespaceBinding &ns = *i;
            LocalBinding *m = ns.second;
            if (m->content->cloneContent == NULL) {
                m->content->cloneContent = m->content->clone();
            }
            LocalBinding *new_b = new LocalBinding(m->accesses, m->content->cloneContent);
            new_b->xplicit = m->xplicit;
            new_e->bindingList.push_back(NamespaceBinding(ns.first, new_b));
        }
        return new_e;
    }


/************************************************************************************
 *
 *  SimpleInstance
 *
 ************************************************************************************/

    
    // Construct a Simple instance of a class. Set the
    // initial value of all slots to uninitialized.
    SimpleInstance::SimpleInstance(JS2Class *type) 
        : JS2Object(SimpleInstanceKind), 
            type(type), 
            slots(new Slot[type->slotCount]),
            dynamicProperties(type->dynamic ? new DynamicPropertyMap() : NULL),
            fWrap(NULL)
    {
        for (uint32 i = 0; i < type->slotCount; i++) {
            slots[i].value = JS2VAL_UNINITIALIZED;
        }
    }

    // gc-mark all contained JS2Objects and visit contained structures to do likewise
    void SimpleInstance::markChildren()
    {
        GCMARKOBJECT(type)
        if (fWrap) {
            GCMARKOBJECT(fWrap->compileFrame);
            GCMARKOBJECT(fWrap->env);
            if (fWrap->bCon)
                fWrap->bCon->mark();
        }
        if (slots) {
            ASSERT(type);
            for (uint32 i = 0; (i < type->slotCount); i++) {
                GCMARKVALUE(slots[i].value);
            }
        }
        if (dynamicProperties) {
            for (DynamicPropertyIterator dpi = dynamicProperties->begin(), dpend = dynamicProperties->end(); (dpi != dpend); dpi++) {
                DynamicPropertyBinding *dpb = *dpi;
                GCMARKVALUE(dpb->v.value);
            }        
        }
    }

 /************************************************************************************
 *
 *  AlienInstance
 *
 ************************************************************************************/

    bool AlienInstance::readProperty(Multiname *m, js2val *rval)
    {
        return false;
    }

    void AlienInstance::writeProperty(Multiname *m, js2val rval)
    {
    }

 /************************************************************************************
 *
 *  Getter
 *
 ************************************************************************************/

    void Getter::mark()
    {
        GCMARKOBJECT(type); 
    }

 /************************************************************************************
 *
 *  Setter
 *
 ************************************************************************************/

    void Setter::mark()
    {
        GCMARKOBJECT(type); 
    }

 /************************************************************************************
 *
 *  PrototypeInstance
 *
 ************************************************************************************/

    PrototypeInstance::PrototypeInstance(JS2Metadata *meta, JS2Object *parent, JS2Class *type) 
        : JS2Object(PrototypeInstanceKind), parent(parent), type(type) 
    {
        // Add prototype property
        writeProperty(meta, meta->engine->prototype_StringAtom, OBJECT_TO_JS2VAL(parent), 0);
    }


    // gc-mark all contained JS2Objects and visit contained structures to do likewise
    void PrototypeInstance::markChildren()
    {
        GCMARKOBJECT(parent)
        for (DynamicPropertyIterator dpi = dynamicProperties.begin(), dpend = dynamicProperties.end(); (dpi != dpend); dpi++) {
            DynamicPropertyBinding *dpb = *dpi;
            GCMARKVALUE(dpb->v.value);
        }        
    }


 /************************************************************************************
 *
 *  FunctionInstance
 *
 ************************************************************************************/

    FunctionInstance::FunctionInstance(JS2Metadata *meta, JS2Object *parent, JS2Class *type)
     : PrototypeInstance(meta, parent, type), fWrap(NULL) 
    {
    }


    // gc-mark all contained JS2Objects and visit contained structures to do likewise
    void FunctionInstance::markChildren()
    {
        PrototypeInstance::markChildren();
        if (fWrap) {
            GCMARKOBJECT(fWrap->env);
            GCMARKOBJECT(fWrap->compileFrame);
            if (fWrap->bCon)
                fWrap->bCon->mark();
        }
    }


/************************************************************************************
 *
 *  MethodClosure
 *
 ************************************************************************************/

    // gc-mark all contained JS2Objects and visit contained structures to do likewise
    void MethodClosure::markChildren()     
    { 
        GCMARKVALUE(thisObject);
        GCMARKOBJECT(method->fInst)
    }

/************************************************************************************
 *
 *  Frame
 *
 ************************************************************************************/

    // Allocate a new temporary variable in this frame and stick it
    // on the list (which may need to be created) for gc tracking.
    uint16 NonWithFrame::allocateTemp()
    {
        if (temps == NULL)
            temps = new std::vector<js2val>;
        uint16 result = (uint16)(temps->size());
        temps->push_back(JS2VAL_VOID);
        return result;
    }


    // gc-mark all contained JS2Objects and visit contained structures to do likewise
    void NonWithFrame::markChildren()
    {
        GCMARKOBJECT(pluralFrame)
        for (LocalBindingIterator bi = localBindings.begin(), bend = localBindings.end(); (bi != bend); bi++) {
            LocalBindingEntry *lbe = *bi;
            for (LocalBindingEntry::NS_Iterator i = lbe->begin(), end = lbe->end(); (i != end); i++) {
                LocalBindingEntry::NamespaceBinding ns = *i;
                ns.second->content->mark();
            }
        }            
        if (temps) {
            for (std::vector<js2val>::iterator i = temps->begin(), end = temps->end(); (i != end); i++)
                GCMARKVALUE(*i);
        }
    }


 /************************************************************************************
 *
 *  GlobalObject
 *
 ************************************************************************************/

    // gc-mark all contained JS2Objects and visit contained structures to do likewise
    void GlobalObject::markChildren()
    {
        NonWithFrame::markChildren();
        GCMARKOBJECT(internalNamespace)
        for (DynamicPropertyIterator dpi = dynamicProperties.begin(), dpend = dynamicProperties.end(); (dpi != dpend); dpi++) {
            DynamicPropertyBinding *dpb = *dpi;
            GCMARKVALUE(dpb->v.value);
        }        
    }


 /************************************************************************************
 *
 *  ParameterFrame
 *
 ************************************************************************************/

    void ParameterFrame::instantiate(Environment *env)
    {
        env->instantiateFrame(pluralFrame, this);
    }

    // Assume that instantiate has been called, the plural frame will contain
    // the cloned Variables assigned into this (singular) frame. Use the 
    // incoming values to initialize the positionals.
    void ParameterFrame::assignArguments(JS2Metadata *meta, JS2Object *fnObj, js2val *argBase, uint32 argCount)
    {
        Multiname mn(NULL, meta->publicNamespace);

        ASSERT(pluralFrame->kind == ParameterKind);
        ParameterFrame *plural = checked_cast<ParameterFrame *>(pluralFrame);
        ASSERT((plural->positionalCount == 0) || (plural->positional != NULL));
        
        PrototypeInstance *argsObj = NULL;
        RootKeeper rk(&argsObj);
        argsObj = new PrototypeInstance(meta, meta->objectClass->prototype, meta->objectClass);

        // Add the 'arguments' property
        String *name = &meta->world.identifiers["arguments"];
        ASSERT(localBindings[*name] == NULL);
        LocalBindingEntry *lbe = new LocalBindingEntry(*name);
        LocalBinding *sb = new LocalBinding(ReadAccess, new Variable(meta->arrayClass, OBJECT_TO_JS2VAL(argsObj), true));
        lbe->bindingList.push_back(LocalBindingEntry::NamespaceBinding(meta->publicNamespace, sb));

        uint32 i;
        for (i = 0; (i < argCount); i++) {
            if (i < plural->positionalCount) {
                ASSERT(plural->positional[i]->cloneContent);
                ASSERT(plural->positional[i]->cloneContent->kind == Member::Variable);
                (checked_cast<Variable *>(plural->positional[i]->cloneContent))->value = argBase[i];
            }
            argsObj->writeProperty(meta, meta->engine->numberToString(i), argBase[i], 0);
        }
        setLength(meta, argsObj, i);
        argsObj->writeProperty(meta, &meta->world.identifiers["callee"], OBJECT_TO_JS2VAL(fnObj), 0);
    }


    // gc-mark all contained JS2Objects and visit contained structures to do likewise
    void ParameterFrame::markChildren()
    {
        NonWithFrame::markChildren();
        GCMARKVALUE(thisObject);
    }


 /************************************************************************************
 *
 *  BlockFrame
 *
 ************************************************************************************/

    void BlockFrame::instantiate(Environment *env)
    {
        if (pluralFrame)
            env->instantiateFrame(pluralFrame, this);
    }


 /************************************************************************************
 *
 *  InstanceMember
 *
 ************************************************************************************/

    // gc-mark all contained JS2Objects and visit contained structures to do likewise
    void InstanceMember::mark()                 
    { 
        GCMARKOBJECT(type);
    }


 /************************************************************************************
 *
 *  InstanceVariable
 *
 ************************************************************************************/

    // An instance variable type could be future'd when a gc runs (i.e. validate
    // has executed, but the pre-eval stage has yet to determine the actual type)

    void InstanceVariable::mark()                 
    { 
        if (type != FUTURE_TYPE)
            GCMARKOBJECT(type);
    }



 /************************************************************************************
 *
 *  InstanceMethod
 *
 ************************************************************************************/

    // gc-mark all contained JS2Objects and visit contained structures to do likewise
    void InstanceMethod::mark()                 
    { 
        GCMARKOBJECT(fInst); 
    }


/************************************************************************************
 *
 *  JS2Object
 *
 ************************************************************************************/

    Pond JS2Object::pond(POND_SIZE, NULL);
    std::list<PondScum **> JS2Object::rootList;

    // Add a pointer to a gc-allocated object to the root list
    // (Note - we hand out an iterator, so it's essential to
    // use something like std::list that doesn't mess with locations)
    JS2Object::RootIterator JS2Object::addRoot(void *t)
    {
        PondScum **p = (PondScum **)t;
        ASSERT(p);
        return rootList.insert(rootList.end(), p);
    }

    // Remove a root pointer
    void JS2Object::removeRoot(RootIterator ri)
    {
        rootList.erase(ri);
    }

    // Mark all reachable objects and put the rest back on the freelist
    uint32 JS2Object::gc()
    {
        pond.resetMarks();
        // Anything on the root list is a pointer to a JS2Object.
        for (std::list<PondScum **>::iterator i = rootList.begin(), end = rootList.end(); (i != end); i++) {
            if (**i) {
                PondScum *p = (**i) - 1;
                ASSERT(p->owner && (p->getSize() >= sizeof(PondScum)) && (p->owner->sanity == POND_SANITY));
                JS2Object *obj = (JS2Object *)(p + 1);
                GCMARKOBJECT(obj)
            }
        }
        return pond.moveUnmarkedToFreeList();
    }

    // Allocate a chunk of size s
    void *JS2Object::alloc(size_t s)
    {
        s += sizeof(PondScum);
        // make sure that the thing is a multiple of 16 bytes
        if (s & 0xF) s += 16 - (s & 0xF);
        ASSERT(s <= 0x7FFFFFFF);
        void *p = pond.allocFromPond(s);
        ASSERT(((ptrdiff_t)p & 0xF) == 0);
        return p;
    }

    // Release a chunk back to it's pond
    void JS2Object::unalloc(void *t)
    {
        PondScum *p = (PondScum *)t - 1;
        ASSERT(p->owner && (p->getSize() >= sizeof(PondScum)) && (p->owner->sanity == POND_SANITY));
        p->owner->returnToPond(p);
    }

    void JS2Object::markJS2Value(js2val v)
    {
        if (JS2VAL_IS_OBJECT(v)) { 
            JS2Object *obj = JS2VAL_TO_OBJECT(v); 
            GCMARKOBJECT(obj);
        }
        else
        if (JS2VAL_IS_STRING(v)) 
            JS2Object::mark(JS2VAL_TO_STRING(v));
        else
        if (JS2VAL_IS_DOUBLE(v)) 
            JS2Object::mark(JS2VAL_TO_DOUBLE(v));
        else
        if (JS2VAL_IS_LONG(v)) 
            JS2Object::mark(JS2VAL_TO_LONG(v));
        else
        if (JS2VAL_IS_ULONG(v)) 
            JS2Object::mark(JS2VAL_TO_ULONG(v));
        else
        if (JS2VAL_IS_FLOAT(v)) 
            JS2Object::mark(JS2VAL_TO_FLOAT(v));
    }

/************************************************************************************
 *
 *  Pond
 *
 ************************************************************************************/

    Pond::Pond(size_t sz, Pond *next) : sanity(POND_SANITY), pondSize(sz + POND_SIZE), pondBase(new uint8[pondSize]), pondBottom(pondBase), pondTop(pondBase), freeHeader(NULL), nextPond(next) 
    {
        /*
         * Make sure the allocation base is at (n mod 16) == 8.
         * That way, each allocated chunk will have it's returned pointer
         * at (n mod 16) == 0 after allowing for the PondScum header at the
         * beginning.
         */
        int32 offset = ((ptrdiff_t)pondBottom % 16);
        if (offset != 8) {
            if (offset > 8)
                pondBottom += 8 + (16 - offset);
            else
                pondBottom += 8 - offset;
        }
        pondTop = pondBottom;
        pondSize -= (pondTop - pondBase);
    }
    
    // Allocate from this or the next Pond (make a new one if necessary)
    void *Pond::allocFromPond(size_t sz)
    {

        // See if there's room left...
        if (sz > pondSize) {
            // If not, try the free list...
            PondScum *p = freeHeader;
            PondScum *pre = NULL;
            while (p) {
                ASSERT(p->getSize() > 0);
                if (p->getSize() >= sz) {
                    if (pre)
                        pre->owner = p->owner;
                    else
                        freeHeader = (PondScum *)(p->owner);
                    p->owner = this;
                    p->resetMark();      // might have lingering mark from previous gc
#ifdef DEBUG
                    memset((p + 1), 0xB7, p->getSize() - sizeof(PondScum));
#endif
                    return (p + 1);
                }
                pre = p;
                p = (PondScum *)(p->owner);
            }
            // ok, then try the next Pond
            if (nextPond == NULL) {
                // there isn't one; run the gc
            uint32 released = JS2Object::gc();
            if (released > sz)
                return JS2Object::alloc(sz - sizeof(PondScum));
                nextPond = new Pond(sz, nextPond);
            }
            return nextPond->allocFromPond(sz);
        }
        // there was room, so acquire it
        PondScum *p = (PondScum *)pondTop;
        p->owner = this;
        p->setSize(sz);
        pondTop += sz;
        pondSize -= sz;
#ifdef DEBUG
        memset((p + 1), 0xB7, sz - sizeof(PondScum));
#endif
        return (p + 1);
    }

    // Stick the chunk at the start of the free list
    uint32 Pond::returnToPond(PondScum *p)
    {
        p->owner = (Pond *)freeHeader;
        uint8 *t = (uint8 *)(p + 1);
#ifdef DEBUG
        memset(t, 0xB3, p->getSize() - sizeof(PondScum));
#endif
        freeHeader = p;
        return p->getSize() - sizeof(PondScum);
    }

    // Clear the mark bit from all PondScums
    void Pond::resetMarks()
    {
        uint8 *t = pondBottom;
        while (t != pondTop) {
            PondScum *p = (PondScum *)t;
            p->resetMark();
            t += p->getSize();
        }
        if (nextPond)
            nextPond->resetMarks();
    }

    // Anything left unmarked is now moved to the free list
    uint32 Pond::moveUnmarkedToFreeList()
    {
        uint32 released = 0;
        uint8 *t = pondBottom;
        while (t != pondTop) {
            PondScum *p = (PondScum *)t;
            if (!p->isMarked() && (p->owner == this))   // (owner != this) ==> already on free list
                released += returnToPond(p);
            t += p->getSize();
        }
        if (nextPond)
            released += nextPond->moveUnmarkedToFreeList();
        return released;
    }

}; // namespace MetaData
}; // namespace Javascript
