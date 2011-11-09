/**************************************************************************************

Copyright © 2004-2010 VMware, Inc. All rights reserved.

Written by Mark Venguerov 2004 - 2010

**************************************************************************************/

#include "qbuild.h"
#include "expr.h"
#include "ftindex.h"
#include "queryop.h"
#include "stmt.h"
#include "parser.h"

using namespace MVStoreKernel;

QueryCtx::QueryCtx(Session *s,const Value *prs,unsigned nprs,const Stmt *st,ulong nsk,ulong md)
	: ses(s),pars(prs),nPars(nprs),stmt(st),nSkip(nsk),mode(md),flg(0),propsReq(NULL),nPropsReq(0),sortReq(NULL),nSortReq(0),nqs(0),ncqs(0) 
{
	if ((md&MODE_FOR_UPDATE)!=0) flg|=QO_FORUPDATE; if ((md&MODE_DELETED)!=0) flg|=QO_DELETED;
	if ((md&MODE_COPY_VALUES)!=0) flg|=QO_VCOPIED; if ((md&MODE_CLASS)!=0) flg|=QO_CLASS;
}

QueryCtx::~QueryCtx()
{
	for (ulong i=0; i<nqs; i++) delete src[i];
}

RC QueryCtx::process(QueryOp *&qop)
{
	qop=NULL; if (stmt==NULL||stmt->top==NULL) return RC_EOF;

	QVar *qv=stmt->top;
	
	sortReq=stmt->orderBy; nSortReq=stmt->nOrderBy;

	RC rc=qv->build(*this,qop); if (rc!=RC_OK) return rc; assert(qop!=NULL);

	QODescr dscr; qop->getOpDescr(dscr);
	if (qv->groupBy==NULL || qv->nGroupBy==0) switch (qv->dtype) {
	case DT_DISTINCT: case DT_DEFAULT:
		if ((dscr.flags&QO_UNIQUE)==0) {
			QueryOp *q=new(ses,0,nPropsReq) Sort(qop,NULL,0,flg|QO_UNIQUE,0,propsReq,nPropsReq);
			if (q==NULL) {delete qop; qop=NULL; return RC_NORESOURCES;}
			qop=q;
		}
		break;
	case DT_DISTINCT_VALUES:
		if (propsReq!=NULL && nPropsReq!=0 || (dscr.flags&QO_UNIQUE)==0) {
			OrderSegQ *os=(OrderSegQ*)alloca(nPropsReq*sizeof(OrderSegQ));
			if (os==NULL) {delete qop; qop=NULL; return RC_NORESOURCES;}
			for (unsigned i=0; i<nPropsReq; i++) {os[i].expr=NULL; os[i].flags=0; os[i].aggop=OP_ALL; os[i].lPref=0; os[i].pid=propsReq[i];}
			QueryOp *q=new(ses,0,nPropsReq) Sort(qop,os,nPropsReq,flg|QO_VUNIQUE,0,NULL,0);
			if (q!=NULL) qop=q; else {delete qop; qop=NULL; return RC_NORESOURCES;}
		}
		break;
	case DT_ALL:
		qop->unique(false); break;
	}
	if (stmt->orderBy!=NULL) rc=sort(qop,dscr,stmt->orderBy,stmt->nOrderBy);
	else if ((dscr.flags&QO_DEGREE)!=0) {
		OrderSegQ ks; ks.pid=PROP_SPEC_ANY; ks.flags=0; ks.var=0; ks.aggop=OP_SET; ks.lPref=0;
		QueryOp *q=new(ses,1,0) Sort(qop,&ks,1,flg,1,NULL,0); if (q==NULL) delete qop; qop=q;
	}
	if (qop==NULL) rc=RC_NORESOURCES;
	else if (rc!=RC_OK) {delete qop; qop=NULL;}
	else {
		if (nSkip!=0) qop->setSkip(nSkip);
		if ((mode&MODE_VERBOSE)!=0 || (ses->getTraceMode()&TRACE_EXEC_PLAN)!=0)
			{SOutCtx buf(ses); qop->print(buf,0); size_t l; byte *p=buf.result(l); ses->trace(1,"%.*s\n",l,p);}
	}
	return rc;
}

RC SetOpVar::build(class QueryCtx& qctx,class QueryOp *&q) const
{
	q=NULL;	assert(type==QRY_UNION||type==QRY_EXCEPT||type==QRY_INTERSECT);
	if (type==QRY_EXCEPT && nVars!=2) return RC_INTERNAL;
	RC rc=RC_OK; QueryOp *qq; const ulong nqs0=qctx.nqs;
	const OrderSegQ *const os=qctx.sortReq; const unsigned nos=qctx.nSortReq;
	OrderSegQ ids; ids.pid=PROP_SPEC_PINID; ids.flags=0; ids.var=0; ids.aggop=OP_SET; ids.lPref=0;
	qctx.sortReq=&ids; qctx.nSortReq=1;
	for (unsigned i=0; i<nVars; i++) {
		if (qctx.nqs>=sizeof(qctx.src)/sizeof(qctx.src[0])) {rc=RC_NORESOURCES; break;}
		if ((rc=vars[i].var->build(qctx,qq))==RC_OK) qctx.src[qctx.nqs++]=qq; else break;
		QODescr dscr; qq->getOpDescr(dscr); if ((rc=qctx.sort(qq,dscr,NULL,0))!=RC_OK) break;
	}
	qctx.sortReq=os; qctx.nSortReq=nos;
	if (rc==RC_OK) {
		if (type!=QRY_EXCEPT) rc=qctx.mergeN(q,&qctx.src[nqs0],nVars,type==QRY_UNION);
		else rc=qctx.merge2(q,qctx.src[nqs0],PROP_SPEC_PINID,qctx.src[nqs0+1],PROP_SPEC_PINID,QRY_EXCEPT);
		if (rc==RC_OK) qctx.nqs-=nVars;
	}
	if (rc!=RC_OK) while (qctx.nqs>nqs0) delete qctx.src[--qctx.nqs];
	return rc;
}

RC JoinVar::build(class QueryCtx& qctx,class QueryOp *&q) const
{
	q=NULL;	RC rc=RC_OK; const ulong nqs0=qctx.nqs;
	assert(type==QRY_JOIN||type==QRY_LEFTJOIN||type==QRY_RIGHTJOIN||type==QRY_OUTERJOIN);
	const bool fTrans=groupBy!=NULL && nGroupBy!=0 || outs!=NULL && nOuts!=0;
	if (fTrans) {
		// merge props
	}
	if (condEJ!=NULL && (condEJ->propID1!=PROP_SPEC_PINID || condEJ->propID2!=PROP_SPEC_PINID)) {
		if ((rc=vars[0].var->build(qctx,qctx.src[qctx.nqs]))==RC_OK) qctx.nqs++; else return rc;
		if ((rc=vars[1].var->build(qctx,qctx.src[qctx.nqs]))==RC_OK) qctx.nqs++; else return rc;
		// choose which condEJ ?
		rc=qctx.merge2(q,qctx.src[nqs0],condEJ->propID1,qctx.src[nqs0+1],condEJ->propID2,(QUERY_SETOP)type,condEJ->next,nConds==1?&cond:conds,nConds);
#if 0
	} else if (vars[0].var->getType()==QRY_COLLECTION /*&& (qctx.req&QRQ_SORT)==0*/) {
		if ((rc=vars[0].var->build(qctx,qctx.src[qctx.nqs]))==RC_OK) qctx.nqs++; else return rc;
		if ((rc=vars[1].var->build(qctx,qctx.src[qctx.nqs]))==RC_OK) qctx.nqs++; else return rc;
		// process vars
		if ((q=new(qctx.ses) HashOp(qctx.ses,qctx.src[nqs0],qctx.src[nqs0+1]))==NULL) rc=RC_NORESOURCES;
#endif
	} else if (nConds>0 && condEJ==NULL) {
		if ((rc=vars[0].var->build(qctx,qctx.src[qctx.nqs]))==RC_OK) qctx.nqs++; else return rc;
		if ((rc=vars[1].var->build(qctx,qctx.src[qctx.nqs]))==RC_OK) qctx.nqs++; else return rc;
		// process vars
		// get properties from conditions?
//		rc=NestedLoop::create(ses,q,src[nqs0],src[nqs0+1],qv->conds,qv->relOp,qctx.flg);
		 qctx.nqs-=2;
	} else {
		if ((rc=vars[0].var->build(qctx,qctx.src[qctx.nqs]))==RC_OK) qctx.nqs++; else return rc;
		if ((rc=vars[1].var->build(qctx,qctx.src[qctx.nqs]))==RC_OK) qctx.nqs++; else return rc;
		// process vars
#if 0
		if ((qctx.req&QRQ_SORT)!=0) {
			assert(orderProps!=NULL);
			if (src[nqs0]->canUseSort(orderProps,nOrderProps)) q=new(ses) HashOp(ses,src[nqs0],src[nqs0+1]);
			else if (src[nqs0+1]->canUseSort(orderProps,nOrderProps)) q=new(ses) HashOp(ses,src[nqs0+1],src[nqs0]);
		}
#endif
		if (q==NULL) rc=qctx.mergeN(q,&qctx.src[nqs0],2,false);
	}
	if (rc==RC_OK)  {
		qctx.nqs-=nVars;
		if (fTrans) rc=qctx.out(q,groupBy,nGroupBy,outs!=NULL?outs[0].vals:NULL,outs!=NULL?outs[0].nValues:0,nHavingConds==1?&havingCond:havingConds,nHavingConds);
	}
	return rc;
}

RC SimpleVar::build(class QueryCtx& qctx,class QueryOp *&q) const
{
	q=NULL;	assert(type==QRY_SIMPLE);
	RC rc=RC_OK; QueryOp *qq,*primary=NULL; const ulong nqs0=qctx.nqs,ncqs0=qctx.ncqs;
	const bool fTrans=groupBy!=NULL && nGroupBy!=0 || outs!=NULL && nOuts!=0;
	if (fTrans) {
		// merge props
	}

	if (subq!=NULL) {
		if (subq->op!=STMT_QUERY || subq->top==NULL) return RC_INVPARAM;
		const Stmt *saveQ=qctx.stmt; qctx.stmt=subq;
// merge propsReq with getProps
		rc=subq->top->build(qctx,q); qctx.stmt=saveQ; if (rc!=RC_OK) return rc;
		return rc!=RC_OK || nConds==0 ? rc : qctx.filter(q,nConds==1?&cond:conds,nConds);
	} else for (ulong i=0; rc==RC_OK && i<nClasses; i++) {
		const ClassSpec &cs=classes[i]; ClassID cid=cs.classID; PID *pids; unsigned nPids,j;
		if ((cid&CLASS_PARAM_REF)!=0) {
			ulong idx=cid&~CLASS_PARAM_REF; if (qctx.pars==NULL||idx>qctx.nPars) return RC_INVPARAM;
			switch (qctx.pars[idx].type) {
			case VT_URIID: cid=qctx.pars[idx].uid; break;
			case VT_REFID:
				if (qctx.nqs>=sizeof(qctx.src)/sizeof(qctx.src[0])) rc=RC_NORESOURCES;
				else if ((qctx.src[qctx.nqs]=new(qctx.ses,1) ArrayScan(qctx.ses,&qctx.pars[idx].id,1,qctx.flg))==NULL) rc=RC_NORESOURCES;
				else {qctx.nqs++; continue;}
				break;
			case VT_ARRAY:
				pids=NULL;
				if (qctx.nqs>=sizeof(qctx.src)/sizeof(qctx.src[0])) rc=RC_NORESOURCES;
				else for (j=nPids=0; j<qctx.pars[idx].length; j++) if (qctx.pars[idx].varray[j].type==VT_REFID) {
					if (pids==NULL && (pids=(PID*)qctx.ses->malloc((qctx.pars[idx].length-j)*sizeof(PID)))==NULL) {rc=RC_NORESOURCES; break;}
					pids[nPids++]=qctx.pars[idx].varray[j].id;
				}
				if (rc==RC_OK) {
					if (pids==NULL) continue; if (nPids>1) qsort(pids,nPids,sizeof(PID),cmpPIDs);
					if ((qctx.src[qctx.nqs]=new(qctx.ses,nPids) ArrayScan(qctx.ses,pids,nPids,qctx.flg))==NULL) rc=RC_NORESOURCES;
					else {qctx.nqs++; qctx.ses->free(pids); continue;}
				}
				if (pids!=NULL) qctx.ses->free(pids);
				break;
			case VT_STMT:
				if (qctx.pars[idx].stmt->getOp()!=STMT_QUERY) rc=RC_INVPARAM;
				else if (qctx.nqs>=sizeof(qctx.src)/sizeof(qctx.src[0])) rc=RC_NORESOURCES;
				else rc=RC_INTERNAL;	// NIY
				break;
			case VT_REFIDPROP:
			case VT_REFPROP:
				//...
			default: rc=RC_TYPE; break;
			}
			if (rc!=RC_OK) break;
		}
		Class *cls=qctx.ses->getStore()->classMgr->getClass(cid); if (cls==NULL) {rc=RC_NOTFOUND; break;}
		const Stmt *cqry=cls->getQuery(); ClassIndex *cidx=cls->getIndex(); const ulong cflg=cls->getFlags(); IndexScan *is=NULL;
		if (qctx.nqs>=sizeof(qctx.src)/sizeof(qctx.src[0])) rc=RC_NORESOURCES;
		else if ((cflg&(CLASS_INDEXED|CLASS_VIEW))!=CLASS_INDEXED || (qctx.mode&MODE_DELETED)!=0 && ((cflg&CLASS_SDELETE)==0||cs.nParams!=0&&cidx!=NULL)) {
			if (qctx.ncqs>=sizeof(qctx.condQs)/sizeof(qctx.condQs[0])) rc=RC_NORESOURCES;
			else if (cqry!=NULL) {
#ifdef _DEBUG
				if ((qctx.mode&(MODE_CLASS|MODE_DELETED))==0) {char *s=qctx.stmt->toString(); report(MSG_WARNING,"Using non-indexed class: %.512s\n",s); qctx.ses->free(s);}
#endif
				QueryWithParams &qs=qctx.condQs[qctx.ncqs++]; qs.params=NULL; qs.nParams=0;
				if ((qs.qry=cqry->clone(STMT_QUERY,qctx.ses))==NULL) rc=RC_NORESOURCES; else {qs.params=(Value*)cs.params; qs.nParams=cs.nParams;}
			}
		} else if (cidx==NULL) {
			if ((qctx.src[qctx.nqs++]=new(qctx.ses) ClassScan(qctx.ses,cls,qctx.flg))==NULL) rc=RC_NORESOURCES;
			else if (cqry!=NULL && cqry->hasParams() && cs.params!=NULL && cs.nParams!=0) {
				if (qctx.ncqs>=sizeof(qctx.condQs)/sizeof(qctx.condQs[0])) rc=RC_NORESOURCES;
				else {
					QueryWithParams &qs=qctx.condQs[qctx.ncqs++]; qs.params=NULL; qs.nParams=0;
					if ((qs.qry=cqry->clone(STMT_QUERY,qctx.ses))==NULL) rc=RC_NORESOURCES; else {qs.params=(Value*)cs.params; qs.nParams=cs.nParams;}
				}
			}
		} else {
			assert(cqry!=NULL && cqry->top!=NULL && cqry->top->getType()==QRY_SIMPLE && ((SimpleVar*)cqry->top)->condIdx!=NULL);
			ushort flags=SCAN_EXACT; ulong i=0,nRanges=0; const Value *param; const unsigned nSegs=((SimpleVar*)cqry->top)->nCondIdx;
			struct IdxParam {const Value *param; uint32_t idx;} *iparams=(IdxParam*)alloca(nSegs*sizeof(IdxParam));
			const Value **curValues=(const Value**)alloca(nSegs*sizeof(Value*)),*cv;
			if (iparams==NULL || curValues==NULL) rc=RC_NORESOURCES;
			else {
				if (cs.nParams==0) flags&=~SCAN_EXACT;
				else for (CondIdx *pci=((SimpleVar*)cqry->top)->condIdx; pci!=NULL; pci=pci->next,++i) {
					if (pci->param>=cs.nParams || (param=&cs.params[pci->param])->type==VT_RANGE && param->varray[0].type==VT_ANY && param->varray[1].type==VT_ANY) {
						iparams[i].param=NULL; flags&=~SCAN_EXACT;
					} else if (param->type==VT_ANY) {
						if ((pci->ks.flags&(ORD_NULLS_BEFORE|ORD_NULLS_AFTER))!=0 && nRanges==0) nRanges=1;
						iparams[i].param=NULL; flags&=~SCAN_EXACT;
					} else {
						if (param->type==VT_PARAM) {
							if (param->length!=0 || param->refPath.refN>=qctx.nPars) {rc=RC_INVPARAM; break;}
							param=&qctx.pars[param->refPath.refN];
						}
						iparams[i].param=param; iparams[i].idx=0; ulong nVals=1;
						if (param->type==VT_ARRAY) nVals=param->length;
						else if (param->type==VT_COLLECTION) nVals=param->nav->count();
						if (nVals>1) {
							if (pci->ks.op!=OP_EQ && pci->ks.op!=OP_IN && pci->ks.op!=OP_BEGINS) {rc=RC_TYPE; break;}
							flags&=~SCAN_EXACT;
						}
						if (nVals>nRanges) nRanges=nVals;
					}
				}
				if (rc==RC_OK && (is=new(qctx.ses,nRanges,*cidx) IndexScan(qctx.ses,*cidx,flags,nRanges,qctx.flg))==NULL) rc=RC_NORESOURCES;
				for (ulong i=0; rc==RC_OK && i<nRanges; i++) {
					bool fRange=false; byte op;
					for (ulong k=0; k<nSegs; k++) if ((cv=curValues[k]=iparams[k].param)!=NULL) {
						if (cv->type==VT_ARRAY) {
							if (iparams[k].idx<cv->length) cv=&cv->varray[iparams[k].idx++]; else cv=iparams[k].param=NULL;
						} else if (cv->type==VT_COLLECTION) {
							if ((cv=cv->nav->navigate(i==0?GO_FIRST:GO_NEXT))==NULL) iparams[k].param=NULL;
						}
						if ((curValues[k]=cv)==NULL) is->flags&=~SCAN_EXACT;
						else switch (cv->type) {
						case VT_RANGE:
							if (cidx->getIndexSegs()[k].op==OP_IN) {fRange=true; is->flags&=~SCAN_EXACT;} else rc=RC_TYPE;
							break;
						case VT_EXPR: case VT_STMT: case VT_EXPRTREE: case VT_PARAM: case VT_VARREF:
							rc=RC_TYPE; break;
						default:
							op=cidx->getIndexSegs()[k].op;
							if (op!=OP_EQ && op!=OP_IN) {is->flags&=~SCAN_EXACT; if (op!=OP_BEGINS) fRange=true;}
							break;
						}
					}
					if (rc!=RC_OK || (rc=((SearchKey*)(is+1))[i*2].toKey(curValues,nSegs,cidx->getIndexSegs(),0,qctx.ses))!=RC_OK) break;
					if (!fRange) ((SearchKey*)(is+1))[i*2+1].copy(((SearchKey*)(is+1))[i*2]);
					else if ((rc=((SearchKey*)(is+1))[i*2+1].toKey(curValues,nSegs,cidx->getIndexSegs(),1,qctx.ses))!=RC_OK) break;
				}
				if (rc==RC_OK) {
					QVar *cqv=cqry->top;
					if (cqv->nConds>0 && cqry->hasParams()) {
						const Expr *const *pc=cqv->nConds==1?&cqv->cond:cqv->conds; cqry=NULL;
						for (unsigned i=0; i<cqv->nConds; i++) if ((pc[i]->getFlags()&EXPR_PARAMS)!=0) {
							if (cqry==NULL) {
								if (qctx.ncqs>=sizeof(qctx.condQs)/sizeof(qctx.condQs[0]) || (cqry=new(qctx.ses) Stmt(0,qctx.ses))==NULL) {rc=RC_NORESOURCES; break;}
								if (((Stmt*)cqry)->addVariable()==0xFF) {((Stmt*)cqry)->destroy(); rc=RC_NORESOURCES; break;}
								QueryWithParams &qs=qctx.condQs[qctx.ncqs++]; qs.qry=(Stmt*)cqry; qs.params=(Value*)cs.params; qs.nParams=cs.nParams;
							}
							if (i+1==cqv->nConds) {
								if ((cqry->top->cond=Expr::clone(pc[i],qctx.ses))!=NULL) cqry->top->nConds=1;
								else {((Stmt*)cqry)->destroy(); rc=RC_NORESOURCES; break;}
							} else {
								if (cqry->top->conds==NULL && (cqry->top->conds=new(qctx.ses) Expr*[cqv->nConds])==NULL 
									|| (cqry->top->conds[cqry->top->nConds]=Expr::clone(pc[i],qctx.ses))==NULL)
										{((Stmt*)cqry)->destroy(); rc=RC_NORESOURCES; break;}
								cqry->top->nConds++;
							}
						}
					}
					if (rc!=RC_OK) delete is;
					else {
						qctx.src[qctx.nqs++]=is;
#if 0
						if ((qctx.req&QRQ_SORT)!=0 && primary==NULL) {
							//assert(orderProps!=NULL);
							//&& ci->expr==NULL && ci->pid==orderProps[0] && ((modf[0]^ci->flags)&ORD_NCASE)==0) primary=is;		// ????????????????????????????????????????????????????????????????
							// check order -> if ok: primary=is; qctx.nqs--;
							// sort ranges!
						}
#endif
					}
				}
			}
		}
		if (cidx==NULL || rc!=RC_OK) cls->release();
	}
	if ((qctx.mode&MODE_DELETED)==0 && rc==RC_OK) for (CondFT *cf=condFT; cf!=NULL; cf=cf->next) {
		if ((rc=qctx.mergeFT(qq,cf))!=RC_OK) break;
		if (qctx.nqs<sizeof(qctx.src)/sizeof(qctx.src[0])) qctx.src[qctx.nqs++]=qq; else {rc=RC_NORESOURCES; break;}
	}
	if (rc==RC_OK) {
		bool fArrayFilter=pids!=NULL && nPids!=0;
		if (qctx.nqs>nqs0) {
			if (qctx.nqs>nqs0+1 && (rc=qctx.mergeN(qq,&qctx.src[nqs0],qctx.nqs-nqs0,false))==RC_OK) {qctx.src[nqs0]=qq; qctx.nqs=nqs0+1;}
			if (rc==RC_OK && primary!=NULL) {if ((qq=new(qctx.ses) HashOp(qctx.ses,primary,qctx.src[nqs0]))!=NULL) {qctx.src[nqs0]=qq; primary=NULL;} else rc=RC_NORESOURCES;}
		} else if (qctx.nqs>=sizeof(qctx.src)/sizeof(qctx.src[0])) rc=RC_NORESOURCES;
		else if (primary!=NULL) {qctx.src[qctx.nqs++]=primary; primary=NULL;}
		else if (fArrayFilter) {
			fArrayFilter=false;
			if ((qctx.src[qctx.nqs]=new(qctx.ses,nPids) ArrayScan(qctx.ses,pids,nPids,qctx.flg))!=NULL) qctx.nqs++; else rc=RC_NORESOURCES;
		} else {
#ifdef _DEBUG
			if ((qctx.mode&(MODE_CLASS|MODE_DELETED))==0) {char *s=qctx.stmt->toString(); report(MSG_WARNING,"Full scan query: %.512s\n",s); qctx.ses->free(s);}
#endif
			qctx.src[qctx.nqs]=new(qctx.ses) FullScan(qctx.ses,(qctx.mode&(MODE_CLASS|MODE_NODEL))==QO_CLASS?HOH_HIDDEN:(qctx.mode&MODE_DELETED)!=0?HOH_DELETED<<16|HOH_DELETED|HOH_HIDDEN:HOH_DELETED|HOH_HIDDEN,qctx.flg);
			if (qctx.src[qctx.nqs]!=NULL) qctx.nqs++; else rc=RC_NORESOURCES;
		}
		if (rc==RC_OK) {
			if (fArrayFilter) {if ((qq=new(nPids,qctx.ses) ArrayFilter(qctx.ses,qctx.src[nqs0],pids,nPids))!=NULL) qctx.src[nqs0]=qq; else rc=RC_NORESOURCES;}
			if (rc==RC_OK && path!=NULL && nPathSeg!=0) {
				const PathSeg *ps=path; const Value *pars=qctx.pars;
				if ((qctx.flg&QO_VCOPIED)!=0) {
					bool fParams=false;
					if ((ps=new(qctx.ses) PathSeg[nPathSeg])==NULL) rc=RC_NORESOURCES;
					else {
						memcpy((PathSeg*)ps,path,nPathSeg*sizeof(PathSeg));
						for (unsigned i=0; i<nPathSeg; i++) if (ps[i].filter!=NULL) {
							fParams=true;
							if ((((PathSeg*)ps)[i].filter=Expr::clone((Expr*)ps[i].filter,qctx.ses))==NULL) {qctx.ses->free((void*)ps); rc=RC_NORESOURCES; fParams=false; break;}
						}
						if (fParams && pars!=NULL && (rc=copyV(pars,qctx.nPars,*(Value**)&pars,qctx.ses))!=RC_OK) qctx.ses->free((void*)ps);	// free filters
					}
				}
				if (rc==RC_OK) {
					QODescr qd; qctx.src[nqs0]->getOpDescr(qd);
					if (BIN<PropertyID>::find(ps[0].pid,qd.props,qd.nProps)==NULL) {
						QueryOp *q=new(qctx.ses,1) BodyOp(qctx.ses,qctx.src[nqs0],&ps[0].pid,1,qctx.flg);
						if (q!=NULL) qctx.src[nqs0]=q; else rc=RC_NORESOURCES;
					}
					if ((qq=new(qctx.ses) PathOp(qctx.ses,qctx.src[nqs0],ps,nPathSeg,pars,qctx.nPars,(qctx.flg&QO_VCOPIED)!=0))!=NULL) qctx.src[nqs0]=qq; else rc=RC_NORESOURCES;
				}
			}
		}
	}
	if (rc!=RC_OK) {
		if (primary!=NULL) delete primary;
		while (qctx.nqs>nqs0) delete qctx.src[--qctx.nqs];
		while (qctx.ncqs>ncqs0) {QueryWithParams &qs=qctx.condQs[--qctx.ncqs]; if (qs.qry!=NULL) qs.qry->destroy();}
	} else {
		if ((q=qctx.src[nqs0])!=NULL && (nConds!=0 || condIdx!=NULL || qctx.ncqs>ncqs0)) rc=qctx.filter(q,nConds==1?&cond:conds,nConds,condIdx,qctx.ncqs-ncqs0);
		if (rc==RC_OK && fTrans)
			rc=qctx.out(q,groupBy,nGroupBy,outs!=NULL?outs[0].vals:NULL,outs!=NULL?outs[0].nValues:0,nHavingConds==1?&havingCond:havingConds,nHavingConds);
	}
	qctx.nqs=nqs0; qctx.ncqs=ncqs0; return rc;		//???
}

RC QueryCtx::sort(QueryOp *&qop,QODescr& dscr,const OrderSegQ *os,unsigned no)
{
	if (os==NULL || no==1 && (os->flags&ORDER_EXPR)==0 && os->pid==PROP_SPEC_PINID) no=0;
	if (no==0 && (dscr.flags&QO_PIDSORT)!=0) return RC_OK;

	unsigned nP=0; bool fReverse=false; RC rc=RC_OK;
	if (dscr.sort!=NULL && no!=0) for (unsigned f;;nP++)
		if (nP>=no) return RC_OK;
		else if (dscr.sort[nP].pid!=os[nP].pid || (dscr.sort[nP].flags&ORDER_EXPR)!=0) break;
		else if ((f=(dscr.sort[nP].flags^os[nP].flags)&SORT_MASK)==0) {if (fReverse) break;}
		else if ((dscr.flags&QO_REVERSIBLE)==0 || (f&~ORD_DESC)!=0) break;
		else /*if (nP==0) fReverse=true; else */ if (!fReverse) break;
	//if (fReverse) -> mark qop to reverse order!

	try {
		PropertyID *pi=(PropertyID*)propsReq; unsigned nps=nPropsReq; bool fAll=false,fDel=false;
		if (no==1 && (os->flags&ORDER_EXPR)==0 && (pi==NULL || nps==0)) {pi=(PropertyID*)&os->pid; nps=1;}
		else if (no!=0) {
			if ((pi=(PropertyID*)alloca((no+nps)*sizeof(PropertyID)))==NULL) return RC_NORESOURCES;
			if (propsReq!=NULL && nps!=0) memcpy(pi,propsReq,nps*sizeof(PropertyID));
			for (unsigned i=0; i<no; i++) if ((os[i].flags&ORDER_EXPR)!=0) {
				const PropertyID *ep; unsigned nep; os[i].expr->getExtRefs(0,ep,nep);
				if (ep!=NULL && nep!=0) {
					if (nep!=1) {
						const PropertyID *pi2=NULL; if (!fDel) {pi2=pi; pi=NULL;}
						if ((rc=Expr::merge(pi2,nps,ep,nep,pi,nps,ses))!=RC_OK) break;
						if (pi==NULL) pi=(PropertyID*)pi2;
					} else if ((rc=BIN<PropertyID>::insert(pi,nps,*ep&STORE_MAX_PROPID,*ep&STORE_MAX_PROPID,fDel?ses:NULL))!=RC_OK) break;
				}
			} else if (os[i].pid==PROP_SPEC_PINID) {no=i+1; break;}
			else if ((rc=BIN<PropertyID>::insert(pi,nps,os[i].pid,os[i].pid,fDel?ses:NULL))!=RC_OK) break;
		}
		if (rc==RC_OK && (dscr.flags&QO_ALLPROPS)==0 && (pi!=NULL && nps!=0 || fAll)) {
			bool fBody=true; const PropertyID *pp=dscr.props,*pp2;
			if (pp!=NULL && !fAll) for (unsigned i=0,l=dscr.nProps; ;i++)
				if (i>=nps) {fBody=false; break;} 
				else if (l==0 || (pp2=BIN<PropertyID>::find(pi[i]&STORE_MAX_PROPID,pp,l))==NULL) break;
				else {l-=unsigned(pp2-pp)+1; pp=pp2+1;}
			if (fBody) {QueryOp *q=new(ses,nps) BodyOp(ses,qop,pi,nps,flg); if (q!=NULL) qop=q; else rc=RC_NORESOURCES;}
		}
		if (rc==RC_OK) {
			Sort *sort=new(ses,no,nps) Sort(qop,os,no,flg,nP,pi,nps);
			if (sort!=NULL) qop=sort; else rc=RC_NORESOURCES;
		}
		if (fDel) ses->free(pi);
	} catch (RC rc2) {rc=rc2;}
	return rc;
}

RC QueryCtx::mergeN(QueryOp *&res,QueryOp **o,unsigned no,bool fOr)
{
	res=NULL; if (o==NULL || no<2) return RC_INTERNAL;
	MergeIDOp *q=new(ses,no) MergeIDOp(ses,flg,fOr); if (q==NULL) return RC_NORESOURCES;
	for (ulong i=0; i<no; i++) {
		QueryOp *qop=o[i]; RC rc;
		if (o!=NULL) {
			QODescr qp; qop->getOpDescr(qp);
			if ((qp.flags&(QO_PIDSORT|QO_UNIQUE))!=(QO_PIDSORT|QO_UNIQUE) && (rc=sort(qop,qp,NULL,0))!=RC_OK)
				{delete q; return rc;}
			q->ops[q->nOps].qop=qop; q->ops[q->nOps].state=QOS_ADV; q->nOps++;
		} else if (!fOr) {delete q; return RC_EOF;}
	}
	if (q->nOps==0) {delete q; return RC_EOF;}
	res=q; return RC_OK;
	return RC_OK;
}

RC QueryCtx::merge2(QueryOp *&res,QueryOp *qop1,PropertyID pid1,QueryOp *qop2,PropertyID pid2,QUERY_SETOP qo,const CondEJ *cej,const Expr *const *c,ulong nc)
{
	res=NULL; if (qop1==NULL || qop2==NULL) return RC_EOF;
	ulong fUni=0; MergeOp *mo; RC rc; OrderSegQ os; os.flags=0; os.var=0; os.aggop=OP_ALL; os.lPref=0; assert(qo!=QRY_UNION && qo!=QRY_INTERSECT);
	QODescr qp1; qop1->getOpDescr(qp1);
	if ((qp1.sort==NULL || qp1.sort[0].pid!=pid1 || (qp1.sort[0].flags&ORD_DESC)!=0) && (pid1!=PROP_SPEC_PINID||(qp1.flags&QO_PIDSORT)==0)) {
		os.pid=pid1; if ((rc=sort(qop1,qp1,&os,1))!=RC_OK) {delete qop1; delete qop2; return rc;}
	} else if (pid1!=PROP_SPEC_PINID) qop1->unique(false);
	qop1->getOpDescr(qp1); if ((qp1.flags&QO_UNIQUE)!=0 && pid1==PROP_SPEC_PINID) fUni|=QOS_UNI1;
	QODescr qp2; qop2->getOpDescr(qp2);
	if ((qp2.sort==NULL || qp2.sort[0].pid!=pid2 || (qp2.sort[0].flags&ORD_DESC)!=0) && (pid2!=PROP_SPEC_PINID||(qp2.flags&QO_PIDSORT)==0)) {
		os.pid=pid2; if ((rc=sort(qop2,qp2,&os,1))!=RC_OK) {delete qop1; delete qop2; return rc;}
	} else if (pid2!=PROP_SPEC_PINID) qop2->unique(false);
	qop2->getOpDescr(qp2); if ((qp2.flags&QO_UNIQUE)!=0 && pid2==PROP_SPEC_PINID) fUni|=QOS_UNI2;
	if ((mo=new(ses) MergeOp(ses,qop1,pid1,qop2,pid2,qo,flg))==NULL) return RC_NORESOURCES;
	if (mo!=NULL) {
		mo->state|=fUni;
		if (cej!=NULL) {
			//...
		}
		if (c!=NULL && nc>0) {
			//...
		}
	}
	res=mo; return RC_OK;
}

RC QueryCtx::mergeFT(QueryOp *&res,const CondFT *cft)
{
	StringTokenizer q(cft->str,strlen(cft->str),false); const FTLocaleInfo *loc=ses->getStore()->ftMgr->getLocale();
	const char *pW; size_t lW; char buf[256]; bool fStop=false,fFlt=(cft->flags&QFT_FILTER_SW)!=0; RC rc=RC_OK; res=NULL;
	QueryOp *qopsbuf[20],**qops=qopsbuf; ulong nqops=0,xqops=20;
	while ((pW=q.nextToken(lW,loc,buf,sizeof(buf)))!=NULL) {
#if 0
		if (*pW==DEFAULT_PHRASE_DEL) {
			if (lW>2) {
				if ((++pW)[--lW-1]==DEFAULT_PHRASE_DEL) lW--;
				// store exact phrase in lower case to phrase
				StringTokenizer phraseStr(pW,lW); FTQuery **pPhrase=NULL;
				while ((pW=reqStr.nextToken(lW,loc,buf,sizeof(buf)))!=NULL) if (!loc->isStopWord(StrLen(pW,lW))) {
					if (loc->stemmer!=NULL) pW=loc->stemmer->process(pW,lW,buf);
					if ((p=(char*)ses->malloc(lW+1))!=NULL) {
						memcpy(p,pW,lW); p[lW]='\0'; StrLen word(p,lW); 
						FTQuery *ft=new FTQuery(word);
						if (ft==NULL) ses->free(p);
						else if (pPhrase!=NULL) {*pPhrase=ft; pPhrase=&ft->next;}
						else {
							FTQuery *ph=*pQuery=new FTQuery(ft); 
							if (ph==NULL) delete ft; else {pQuery=&ph->next; pPhrase=&ft->next;}
						}
					}
				}
			}
		} else 
#endif
		if (lW>1 && (!fFlt || !(fStop=loc->isStopWord(StrLen(pW,lW)))) || q.isEnd() && (lW>1 || nqops==0)) {
			if (loc->stemmer!=NULL) pW=loc->stemmer->process(pW,lW,buf);
			FTScan *ft=new(ses,cft->nPids,lW+1) FTScan(ses,pW,lW,cft->pids,cft->nPids,flg,cft->flags,fStop);
			if (ft==NULL) {rc=RC_NORESOURCES; break;}
			if (nqops>=xqops) {
				if (qops!=qopsbuf) qops=(QueryOp**)ses->realloc(qops,(xqops*=2)*sizeof(QueryOp*));
				else if ((qops=(QueryOp**)ses->malloc((xqops*=2)*sizeof(QueryOp*)))!=NULL) memcpy(qops,qopsbuf,nqops*sizeof(QueryOp*));
				if (qops==NULL) return RC_NORESOURCES;
			}
			qops[nqops++]=ft;
		}
	}
	if (rc==RC_OK) {
		if (nqops<=1) {res=nqops!=0?qops[0]:(rc=RC_EOF,(QueryOp*)0); nqops=0;}
		else if ((rc=mergeN(res,qops,nqops,(mode&MODE_ALL_WORDS)==0))==RC_OK) nqops=0;
	}
	for (ulong i=0; i<nqops; i++) delete qops[i];
	if (qops!=qopsbuf) ses->free(qops);
	return rc;
}

RC QueryCtx::filter(QueryOp *&qop,const Expr *const *conds,unsigned nConds,const CondIdx *condIdx,unsigned ncq)
{
	QODescr dscr; qop->getOpDescr(dscr);
	if ((dscr.flags&QO_ALLPROPS)==0) {
		const PropertyID *pids=NULL; unsigned nPids=0; flg|=QO_NODATA; bool fDel=false; RC rc=RC_OK;
		if (conds!=NULL && nConds!=0) {
			conds[0]->getExtRefs(0,pids,nPids);
			for (unsigned i=1; i<nConds; i++) {
				const PropertyID *ep; unsigned nep; conds[i]->getExtRefs(0,ep,nep);
				if (ep!=NULL && nep!=0) {
					const PropertyID *pi2=NULL; if (!fDel) {pi2=pids; pids=NULL;}
					if ((rc=Expr::merge(pi2,nPids,ep,nep,*(PropertyID**)&pids,nPids,ses))!=RC_OK) {if (fDel) ses->free((void*)pids); return rc;}
					if (pids==NULL) pids=(PropertyID*)pi2; else fDel=true;
				}
			}
		}
		for (const CondIdx *ci=condIdx; ci!=NULL; ci=ci->next) {
			const PropertyID *ip=NULL; unsigned nip=0;
			if (ci->expr==NULL) ip=&ci->ks.propID,nip=1;
			else {
				//...
				// if null-> continue;
			}
			if (pids==NULL) {pids=ip; nPids=nip;}
			else {
				const PropertyID *pi2=NULL; if (!fDel) {pi2=pids; pids=NULL;}
				if ((rc=Expr::merge(pi2,nPids,ip,nip,*(PropertyID**)&pids,nPids,ses))!=RC_OK) {if (fDel) ses->free((void*)pids); return rc;}
				if (pids==NULL) pids=(PropertyID*)pi2; else fDel=true;
			}
		}
		for (unsigned i=0; i<ncq; i++) if (condQs[ncqs-i-1].qry->top!=NULL) {
			PropertyID *qp=NULL; unsigned nqp=0;
			if ((rc=condQs[ncqs-i-1].qry->top->getProps(qp,nqp,ses))!=RC_OK) return rc;
			if (qp!=NULL) {
				if (pids==NULL) {pids=qp; nPids=nqp; fDel=true;}
				else if ((rc=Expr::merge(NULL,0,pids,nPids,qp,nqp,ses))!=RC_OK) {ses->free(qp); if (fDel) ses->free((void*)pids); return rc;}
				else {if (fDel) ses->free((void*)pids); else fDel=true; pids=qp; nPids=nqp;}
			}
		}
		if (pids!=NULL && nPids!=0) {
			bool fBody=true; const PropertyID *pp=dscr.props,*pp2; flg&=~QO_NODATA;
			if (pp!=NULL) for (unsigned i=0,l=dscr.nProps; ;i++)
				if (i>=nPids) {fBody=false; break;} 
				else if (l==0 || (pp2=BIN<PropertyID>::find(pids[i]&STORE_MAX_PROPID,pp,l))==NULL) break;
				else {l-=unsigned(pp2-pp)+1; pp=pp2+1;}
			if (fBody) {
				if (propsReq!=NULL && nPropsReq!=0) {
					// if (!fDel) copy
					// merge to pids, nPids
				}
				QueryOp *q=new(ses,nPids) BodyOp(ses,qop,pids,nPids,flg); if (q!=NULL) qop=q; else rc=RC_NORESOURCES;
			}
			if (fDel) ses->free((void*)pids); if (rc!=RC_OK) return rc;
		}
	}
	Filter *flt=new(ses) Filter(ses,qop,nPars,nqs,flg); if (flt==NULL) return RC_NORESOURCES;
	bool fOK=true; 
	if (pars!=NULL && nPars>0) {
		if ((flg&QO_VCOPIED)!=0) {if (copyV(pars,nPars,flt->params,ses)!=RC_OK) fOK=false;}
		else {flt->params=(Value*)pars; for (unsigned i=0; i<nPars; i++) pars[i].flags=NO_HEAP;}
	}
	if (fOK && conds!=NULL) {
		flt->nConds=nConds;
		if ((flg&QO_VCOPIED)==0) {if (nConds==1) flt->cond=(Expr*)*conds; else flt->conds=(Expr**)conds;}
		else if (nConds==1) {if ((flt->cond=Expr::clone(*conds,ses))==NULL) fOK=false;}
		else if ((flt->conds=new(ses) Expr*[nConds])==NULL) fOK=false;
		else for (ulong i=0; i<nConds; ++i) {if ((flt->conds[i]=Expr::clone(conds[i],ses))==NULL) {fOK=false; break;}}
	}
	if ((flg&QO_CLASS)==0) {
		if ((flg&QO_VCOPIED)==0) flt->condIdx=(CondIdx*)condIdx;
		else if (fOK && condIdx!=NULL) for (CondIdx **ppCondIdx=&flt->condIdx; condIdx!=NULL; condIdx=condIdx->next) {
			CondIdx *newCI=condIdx->clone(ses);
			if (newCI==NULL) {fOK=false; break;}
			else {newCI->next=NULL; *ppCondIdx=newCI; ppCondIdx=&newCI->next; flt->nCondIdx++;}
		}
	}
	if (fOK && ncq>0) {
		assert(ncq<=ncqs);
		if ((flt->queries=new(ses) QueryWithParams[ncq])==NULL) fOK=false;
		else {
			memcpy(flt->queries,&condQs[ncqs-ncq],ncq*sizeof(QueryWithParams)); ncqs-=ncq;
			for (unsigned i=0; i<ncq; i++) {
				QueryWithParams& qwp=flt->queries[i]; RC rc;
				if (qwp.params!=NULL && qwp.nParams!=0) {
					if ((rc=copyV(qwp.params,qwp.nParams,qwp.params,ses))!=RC_OK) {for (;i<ncq;i++) flt->queries[i].params=NULL; fOK=false; break;}
					for (unsigned j=0; j<qwp.nParams; j++) if (qwp.params[j].type==VT_PARAM)
						if (qwp.params[j].refPath.refN<nPars) {qwp.params[j]=pars[qwp.params[j].refPath.refN]; qwp.params[j].flags=NO_HEAP;} else qwp.params[j].setError();
				}
			}
		}
	}
	if (!fOK) {delete flt; return RC_NORESOURCES;}
	qop=flt; return RC_OK;
}

RC QueryCtx::out(QueryOp *&qop,const OrderSegQ *gb,unsigned nG,const Value *outs,unsigned nOuts,const Expr *const *having,unsigned nHaving)
{
	QODescr dscr; qop->getOpDescr(dscr);
	RC rc=sort(qop,dscr,gb,nG);
	if (rc==RC_OK) {
		if (outs==NULL || nOuts==0) {
			// get them from gb/nG
		}
//		QueryOp *q=new(ses,nG,nTrs) AggOp(ses,qop,nG,trs,nTrs,mode);
//		if (q==NULL) rc=RC_NORESOURCES;
//		else {qop=q; if (having!=NULL && nHaving!=0) rc=filter(qop,having,nHaving,NULL,0);}
	}
	return rc;
}
