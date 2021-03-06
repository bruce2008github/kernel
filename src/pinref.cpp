/**************************************************************************************

Copyright © 2004-2014 GoPivotal, Inc. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,  WITHOUT
WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
License for the specific language governing permissions and limitations
under the License.

Written by Mark Venguerov 2010 - 2012

**************************************************************************************/

#include "session.h"
#include "pinref.h"
#include "utils.h"

using namespace AfyKernel;

PINRef::PINRef(ushort si,const byte *p) : stID(si),def(0),count(1)
{
	byte l; if (p==NULL || (l=*p++)==0) throw RC_INVPARAM;
	const byte *end=p+(l-1&0x7F); PageAddr ad; id.ident=STORE_OWNER;
	byte dscr=(l&0x80)!=0?*--end:0,dscr2=(dscr&0x40)!=0?*--end:0;
	if ((dscr&0x03)!=0) {
		if ((dscr&0x02)!=0) {def|=PR_PREF64; afy_dec64(p,prefix);} else {def|=PR_PREF32; afy_dec32(p,prefix);}
		if (p>=end) throw RC_CORRUPTED;
	}
	afy_dec32(p,ad.pageID); if (p>=end) throw RC_CORRUPTED; afy_dec16(p,ad.idx);
	if (dscr!=0) {
		if ((dscr&0x10)!=0) {afy_dec32r(end,count); if (p>end) throw RC_CORRUPTED;}
		if ((dscr2&0x40)!=0) {afy_dec16(p,si); if (p>end) throw RC_CORRUPTED;}
		if ((dscr2&0x80)!=0) {afy_dec32(p,id.ident); if (p>end) throw RC_CORRUPTED;}
		if ((dscr&0x04)!=0) {def|=PR_ADDR; afy_dec32(p,addr.pageID); afy_dec16(p,addr.idx); if (p>end) throw RC_CORRUPTED;}
		if ((dscr&0x08)!=0) {def|=PR_U1; afy_dec32(p,u1); if (p>end) throw RC_CORRUPTED;}
		if ((dscr2&0x20)!=0) {def|=PR_U2; afy_dec32(p,u2); if (p>end) throw RC_CORRUPTED;}
		if ((dscr2&0x01)!=0) {
			def|=PR_PID2; PageAddr ad2; ushort si2=si; id2.ident=id.ident;
			afy_dec32(p,ad2.pageID); afy_dec16(p,ad2.idx); if (p>end) throw RC_CORRUPTED;
			if ((dscr2&0x02)!=0) {afy_dec16(p,si2); afy_dec32(p,id2.ident); if (p>end) throw RC_CORRUPTED;}
			id2.pid=(uint64_t(si2)<<32|ad2.pageID)<<16|ad2.idx;
			if ((dscr2&0x10)!=0) {def|=PR_ADDR2; afy_dec32(p,addr2.pageID); afy_dec16(p,addr2.idx); if (p>end) throw RC_CORRUPTED;}
		}
		if ((dscr&0x20)!=0) def|=PR_FCOLL; if ((dscr2&0x04)!=0) def|=PR_HIDDEN; if ((dscr2&0x08)!=0) def|=PR_SPECIAL;
	} else if (p>end) throw RC_CORRUPTED;
	id.pid=(uint64_t(si)<<32|ad.pageID)<<16|ad.idx;
}

byte PINRef::enc(byte *p) const
{
	byte *const p0=p++; byte dscr=0,dscr2=0; PageAddr ad; ushort si,si2;
	ad.pageID=uint32_t(id.pid>>16); ad.idx=PageIdx(id.pid); si=ushort(id.pid>>48);
	if (si!=stID) dscr2|=0x40; if (id.ident!=STORE_OWNER) dscr2|=0x80;
	if ((def&PR_PREF32)!=0) {afy_enc32(p,uint32_t(prefix)); dscr|=0x01;}
	else if ((def&PR_PREF64)!=0) {afy_enc64(p,prefix); dscr|=0x02;}
	afy_enc32(p,ad.pageID); afy_enc16(p,ad.idx);
	if ((dscr|dscr2|def)!=0) {
		if ((dscr2&0x40)!=0) afy_enc16(p,si);
		if ((dscr2&0x80)!=0) afy_enc32(p,id.ident);
		if (def!=0) {
			if ((def&PR_ADDR)!=0 && addr.defined() && addr!=ad) {dscr|=0x04; afy_enc32(p,addr.pageID); afy_enc16(p,addr.idx);}
			if ((def&PR_U1)!=0 && u1!=~0u) {dscr|=0x08; afy_enc32(p,u1);}
			if ((def&PR_U2)!=0) {dscr2|=0x20; afy_enc32(p,u2);}
			if ((def&PR_PID2)!=0) {
				ad.pageID=uint32_t(id2.pid>>16); ad.idx=PageIdx(id2.pid); si2=ushort(id2.pid>>48);
				afy_enc32(p,ad.pageID); afy_enc16(p,ad.idx); dscr2|=0x01;
				if (si2!=si || id2.ident!=id.ident) {dscr2|=0x02; afy_enc16(p,si2); afy_enc16(p,id2.ident);}
				if ((def&PR_ADDR2)!=0 && addr2.defined() && (addr2.pageID!=ad.pageID || addr2.idx!=ad.idx))
					{dscr2|=0x10; afy_enc32(p,addr2.pageID); afy_enc16(p,addr2.idx);}
			}
			if ((def&PR_COUNT)!=0 && count!=1) {dscr|=0x10; afy_enc32r(p,count);}
			if ((def&PR_FCOLL)!=0) dscr|=0x20; if ((def&PR_HIDDEN)!=0) dscr2|=0x04; if ((def&PR_SPECIAL)!=0) dscr2|=0x08;
		}
		if (dscr2!=0) {*p++=dscr2; dscr|=0x40;} if (dscr!=0) *p++=dscr;
	}
	*p0=byte(p-p0)|(dscr!=0?0x80:0); return byte(p-p0);
}

RC PINRef::getPID(const byte *p,ushort si,PID& id,PageAddr *paddr)
{
	const byte l=*p++,*end=p+(l-1&0x7F); PageAddr ad,ad2; id.ident=STORE_OWNER;
	byte dscr=(l&0x80)!=0?*--end:0,dscr2=(dscr&0x40)!=0?*--end:0;
	if ((dscr&0x01)!=0) {afy_adv32(p);} else if ((dscr&0x02)!=0) {afy_adv32(p);}
	if (p>=end) return RC_CORRUPTED;
	afy_dec32(p,ad.pageID); afy_dec16(p,ad.idx); ad2=ad;
	if ((dscr&0x80)!=0) {
		if (p>=end) return RC_CORRUPTED;
		if ((dscr2&0x40)!=0) afy_dec16(p,si);
		if ((dscr2&0x80)!=0) {if (p>end) return RC_CORRUPTED; afy_dec32(p,id.ident);}
		if (paddr!=NULL && (dscr&0x04)!=0) {if (p>end) return RC_CORRUPTED; afy_dec32(p,ad2.pageID); afy_dec16(p,ad2.idx);}
	}
	id.pid=(uint64_t(si)<<32|ad.pageID)<<16|ad.idx;
	if (paddr!=NULL) *paddr=ad2;
	return RC_OK;
}

RC PINRef::adjustCount(byte *p,uint32_t cnt,byte *buf,bool fDec)
{
	byte *const p0=p,l=*p; RC rc=RC_CORRUPTED; uint32_t cnt0=1;
	if ((l&0x80)==0) {
		if (fDec) rc=cnt==1?RC_FALSE:RC_NOTFOUND;
		else {if (p0!=buf) {memcpy(buf,p0,l); p=buf+l;} ++cnt; afy_enc32r(p,cnt); *p=0x30; buf[0]=byte(p-buf+1)|0x80; rc=RC_TRUE;}
	} else {
		const byte dscr=*(p+=l-1&0x7F),*const end=(dscr&0x40)!=0?--p:p;
		if ((dscr&0x10)!=0) afy_dec32r(p,cnt0);
		if (fDec && cnt0<=cnt) rc=cnt==cnt0?RC_FALSE:RC_NOTFOUND;
		else {
			if (fDec) cnt0-=cnt; else cnt0+=cnt;
			byte cntbuf[10],*pp=cntbuf; afy_enc32r(pp,cnt0);
			size_t lnew=pp-cntbuf; rc=RC_OK;
			if (lnew!=end-p) {if (p0!=buf) {memcpy(buf,p0,p-p0); p=buf+(p-p0);} rc=RC_TRUE;}
			memcpy(p,cntbuf,lnew); p+=lnew; if ((dscr&0x40)!=0) *p++=*end;
			*p=dscr|0x30; buf[0]=byte(p-buf+1)|0x80;
		}
	}
	if (rc==RC_NOTFOUND) report(MSG_ERROR,"Invalid descrease count, cnt=%u, decreased by %u\n",cnt0,cnt);
	return rc;
}

int PINRef::cmpPIDs(const byte *p1,const byte *p2)
{
	int c; assert(p1!=NULL && p2!=NULL);
	const byte l1=*p1++,*const e1=p1+(l1-1&0x7F),l2=*p2++,*const e2=p2+(l2-1&0x7F);
	byte d1=(l1&0x80)!=0?e1[-1]:0,d2=(l2&0x80)!=0?e2[-1]:0;
	uint32_t u1,u2; assert((d1&0x03)==(d2&0x03));
	if ((d1&0x02)!=0) {
		uint64_t U1,U2; afy_dec64(p1,U1); afy_dec64(p2,U2);
		if ((c=cmp3(U1,U2))!=0) return c; assert(p1<e1 && p2<e2);
	} else if ((d1&0x01)!=0) {
		afy_dec32(p1,u1); afy_dec32(p2,u2);
		if ((c=cmp3(u1,u2))!=0) return c; assert(p1<e1 && p2<e2);
	}
	afy_dec32(p1,u1); afy_dec32(p2,u2);
	if ((c=cmp3(u1,u2))!=0) return c; assert(p1<e1 && p2<e2);
	uint16_t x1,x2; afy_dec16(p1,x1); afy_dec16(p2,x2);
	if ((c=cmp3(x1,x2))!=0) return c; assert(p1<=e1 && p2<=e2);
	if (((d1|d2)&0x40)!=0) {
		byte dd1=(d1&0x40)!=0?e1[-2]:0,dd2=(d2&0x40)!=0?e2[-2]:0;
		if (((dd1|dd2)&0x40)!=0) {
			if ((dd1&0x40)==0) x1=StoreCtx::get()->storeID; else afy_dec16(p1,x1);
			if ((dd2&0x40)==0) x2=StoreCtx::get()->storeID; else afy_dec16(p2,x2);
			if ((c=cmp3(x1,x2))!=0) return c; assert(p1<e1 && p2<e2);
		}
		if (((dd1|dd2)&0x80)!=0) {
			if ((dd1&0x80)==0) u1=STORE_OWNER; else afy_dec32(p1,u1);
			if ((dd2&0x80)==0) u2=STORE_OWNER; else afy_dec32(p2,u2);
			return cmp3(u1,u2);
		}
	}
	return 0;
}

int __cdecl AfyKernel::cmpPIDs(const void *p1,const void *p2)
{
	return cmpPIDs(*(PID*)p1,*(PID*)p2);
}
