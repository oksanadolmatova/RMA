/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0.  If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Copyright 1997 - July 2008 CWI, August 2008 - 2018 MonetDB B.V.
 */

#define RSTR(somestr) mkCharCE(somestr, CE_UTF8)


#define BAT_TO_SXP(bat,tpe,retsxp,newfun,ptrfun,ctype,naval,memcopy)\
	do {													\
		tpe v; size_t j;									\
		ctype *valptr = NULL;                               \
		tpe* p = (tpe*) Tloc(bat, 0);           \
		retsxp = PROTECT(newfun(BATcount(bat)));		    \
		if (!retsxp) break;                                 \
		valptr = ptrfun(retsxp);                            \
		if (bat->tnonil && !bat->tnil) {                    \
			if (memcopy) {									\
				memcpy(valptr, p,                           \
					BATcount(bat) * sizeof(tpe));           \
			} else {                                        \
				for (j = 0; j < BATcount(bat); j++) {       \
					valptr[j] = (ctype) p[j];               \
				}                                           \
			} 												\
		} else {                                            \
		for (j = 0; j < BATcount(bat); j++) {				\
			v = p[j];                                       \
			if ( is_##tpe##_nil(v))							\
				valptr[j] = naval;	                        \
			else											\
				valptr[j] = (ctype) v;	                    \
		}}													\
	} while (0)

#define BAT_TO_INTSXP(bat,tpe,retsxp,memcopy)						\
	BAT_TO_SXP(bat,tpe,retsxp,NEW_INTEGER,INTEGER_POINTER,int,NA_INTEGER,memcopy)\

#define BAT_TO_REALSXP(bat,tpe,retsxp,memcopy)						\
	BAT_TO_SXP(bat,tpe,retsxp,NEW_NUMERIC,NUMERIC_POINTER,double,NA_REAL,memcopy)\

#define SXP_TO_BAT(tpe,access_fun,na_check)								\
	do {																\
		tpe *p, prev = tpe##_nil; size_t j;								\
		b = COLnew(0, TYPE_##tpe, cnt, TRANSIENT);						\
		if (!b) break;                                                  \
		b->tnil = 0; b->tnonil = 1; b->tkey = 0;						\
		b->tsorted = 1; b->trevsorted = 1;b->tdense = 0;				\
		p = (tpe*) Tloc(b, 0);								\
		for( j = 0; j < cnt; j++, p++){								    \
			*p = (tpe) access_fun(s)[j];							    \
			if (na_check){ b->tnil = 1; 	b->tnonil = 0; 	*p= tpe##_nil;} \
			if (j > 0){													\
				if (b->trevsorted && !is_##tpe##_nil(*p) && (is_##tpe##_nil(prev) || *p > prev)){						\
					b->trevsorted = 0;									\
				} else													\
					if (b->tsorted && !is_##tpe##_nil(prev) && (is_##tpe##_nil(*p) || *p < prev)){ \
						b->tsorted = 0;									\
					}													\
			}															\
			prev = *p;													\
		}																\
		BATsetcount(b, cnt);											\
		BATsettrivprop(b);												\
	} while (0)

static SEXP bat_to_sexp(BAT* b) {
	SEXP varvalue = NULL;
	// TODO: deal with SQL types (DECIMAL/DATE)
	switch (ATOMstorage(b->ttype)) {
		case TYPE_void: {
			size_t i = 0;
			varvalue = PROTECT(NEW_LOGICAL(BATcount(b)));
			if (!varvalue) {
				return NULL;
			}
			for (i = 0; i < BATcount(b); i++) {
				LOGICAL_POINTER(varvalue)[i] = NA_LOGICAL;
			}
			} break;
		case TYPE_bte:
			BAT_TO_INTSXP(b, bte, varvalue, 0);
			break;
		case TYPE_sht:
			BAT_TO_INTSXP(b, sht, varvalue, 0);
			break;
		case TYPE_int:
			// special case: memcpy for int-to-int conversion without NULLs
			BAT_TO_INTSXP(b, int, varvalue, 1);
			break;
#ifdef HAVE_HGE
		case TYPE_hge: /* R's integers are stored as int, so we cannot be sure hge will fit */
			BAT_TO_REALSXP(b, hge, varvalue, 0);
			break;
#endif
		case TYPE_flt:
			BAT_TO_REALSXP(b, flt, varvalue, 0);
			break;
		case TYPE_dbl:
			// special case: memcpy for double-to-double conversion without NULLs
			BAT_TO_REALSXP(b, dbl, varvalue, 1);
			break;
		case TYPE_lng: /* R's integers are stored as int, so we cannot be sure long will fit */
			BAT_TO_REALSXP(b, lng, varvalue, 0);
			break;
		case TYPE_str: { // there is only one string type, thus no macro here
			BUN p, q, j = 0;
			BATiter li = bat_iterator(b);
			varvalue = PROTECT(NEW_STRING(BATcount(b)));
			if (varvalue == NULL) {
				return NULL;
			}
			/* special case where we exploit the duplicate-eliminated string heap */
			if (GDK_ELIMDOUBLES(b->tvheap)) {
				SEXP* sexp_ptrs = GDKzalloc(b->tvheap->free * sizeof(SEXP));
				if (!sexp_ptrs) {
					return NULL;
				}
				BATloop(b, p, q) {
					const char *t = (const char *) BUNtail(li, p);
					ptrdiff_t offset = t - b->tvheap->base;
					if (!sexp_ptrs[offset]) {
						if (strcmp(t, str_nil) == 0) {
							sexp_ptrs[offset] = NA_STRING;
						} else {
							sexp_ptrs[offset] = RSTR(t);
						}
					}
					SET_STRING_ELT(varvalue, j++, sexp_ptrs[offset]);
				}
				GDKfree(sexp_ptrs);
			}
			else {
				if (b->tnonil) {
					BATloop(b, p, q) {
						SET_STRING_ELT(varvalue, j++, RSTR(
							(const char *) BUNtail(li, p)));
					}
				}
				else {
					BATloop(b, p, q) {
						const char *t = (const char *) BUNtail(li, p);
						if (strcmp(t, str_nil) == 0) {
							SET_STRING_ELT(varvalue, j++, NA_STRING);
						} else {
							SET_STRING_ELT(varvalue, j++, RSTR(t));
						}
					}
				}
			}
		} 	break;
	}
	return varvalue;
}

static BAT* sexp_to_bat(SEXP s, int type) {
	BAT* b = NULL;
	BUN cnt = LENGTH(s);
	switch (type) {
	case TYPE_int: {
		if (!IS_INTEGER(s)) {
			return NULL;
		}
		SXP_TO_BAT(int, INTEGER_POINTER, *p==NA_INTEGER);
		break;
	}
	case TYPE_lng: {
		if (!IS_INTEGER(s)) {
			return NULL;
		}
		SXP_TO_BAT(lng, INTEGER_POINTER, *p==NA_INTEGER);
		break;
	}
#ifdef HAVE_HGE
	case TYPE_hge: {
		if (!IS_INTEGER(s)) {
			return NULL;
		}
		SXP_TO_BAT(hge, INTEGER_POINTER, *p==NA_INTEGER);
		break;
	}
#endif
	case TYPE_bte:
	case TYPE_bit: { // only R logical types fit into bit BATs
		if (!IS_LOGICAL(s)) {
			return NULL;
		}
		SXP_TO_BAT(bit, LOGICAL_POINTER, *p==NA_LOGICAL);
		break;
	}
	case TYPE_dbl: {
		if (!IS_NUMERIC(s)) {
			return NULL;
		}
		SXP_TO_BAT(dbl, NUMERIC_POINTER, (ISNA(*p) || isnan(*p) || isinf(*p)));
		break;
	}
	case TYPE_str: {
		SEXP levels;
		size_t j;
		if (!IS_CHARACTER(s) && !isFactor(s)) {
			return NULL;
		}
		b = COLnew(0, TYPE_str, cnt, TRANSIENT);
		if (!b) return NULL;
		b->tnil = 0;
		b->tnonil = 1;
		b->tkey = 0;
		b->tsorted = 0;
		b->trevsorted = 0;
		/* get levels once, since this is a function call */
		levels = GET_LEVELS(s);

		for (j = 0; j < cnt; j++) {
			SEXP rse;
			if (isFactor(s)) {
				int ii = INTEGER(s)[j];
				if (ii == NA_INTEGER) {
					rse = NA_STRING;
				} else {
					rse = STRING_ELT(levels, ii - 1);
				}
			} else {
				rse = STRING_ELT(s, j);
			}
			if (rse == NA_STRING) {
				b->tnil = 1;
				b->tnonil = 0;
				if (BUNappend(b, str_nil, FALSE) != GDK_SUCCEED) {
					BBPreclaim(b);
					b = NULL;
				}
			} else {
				if (BUNappend(b, CHAR(rse), FALSE) != GDK_SUCCEED) {
					BBPreclaim(b);
					b = NULL;
				}
			}
		}
		break;
	}
	}

	if (b) {
		BATsetcount(b, cnt);
		BBPkeepref(b->batCacheid);
	}
	return b;
}
