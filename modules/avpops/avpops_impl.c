/*
 * $Id$
 *
 * Copyright (C) 2004-2006 Voice Sistem SRL
 *
 * This file is part of Open SIP Express Router (openser).
 *
 * openser is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * openser is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 * History:
 * ---------
 *  2004-10-04  first version (ramona)
 *  2005-01-30  "fm" (fast match) operator added (ramona)
 *  2005-01-30  avp_copy (copy/move operation) added (ramona)
 */

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <regex.h>
#include <fnmatch.h>

#include "../../ut.h"
#include "../../dprint.h"
#include "../../usr_avp.h"
#include "../../action.h"
#include "../../ip_addr.h"
#include "../../config.h"
#include "../../dset.h"
#include "../../data_lump.h"
#include "../../data_lump_rpl.h"
#include "../../items.h"
#include "../../parser/parse_from.h"
#include "../../parser/parse_uri.h"
#include "../../mem/mem.h"
#include "avpops_impl.h"
#include "avpops_db.h"


#define avpops_str2int_str(a, b) \
	do { \
		if(a.s==0) \
			b.n = a.len; \
		else \
			b.s = a; \
	} while(0)


static db_key_t  store_keys[6];
static db_val_t  store_vals[6];
static str      empty={"",0};


#define AVP_PRINTBUF_SIZE 1024
static char printbuf[AVP_PRINTBUF_SIZE];

void init_store_avps( char **db_columns)
{
	/* unique user id */
	store_keys[0] = db_columns[0]; /*uuid*/
	store_vals[0].type = DB_STR;
	store_vals[0].nul  = 0;
	/* attribute */
	store_keys[1] = db_columns[1]; /*attribute*/
	store_vals[1].type = DB_STR;
	store_vals[1].nul  = 0;
	/* value */
	store_keys[2] = db_columns[2]; /*value*/
	store_vals[2].type = DB_STR;
	store_vals[2].nul  = 0;
	/* type */
	store_keys[3] = db_columns[3]; /*type*/
	store_vals[3].type = DB_INT;
	store_vals[3].nul  = 0;
	/* user name */
	store_keys[4] = db_columns[4]; /*username*/
	store_vals[4].type = DB_STR;
	store_vals[4].nul  = 0;
	/* domain */
	store_keys[5] = db_columns[5]; /*domain*/
	store_vals[5].type = DB_STR;
	store_vals[5].nul  = 0;
}


/* value 0 - attr value
 * value 1 - attr name
 * value 2 - attr type
 */
static int dbrow2avp(struct db_row *row, struct db_param *dbp, int_str attr,
														int just_val_flags)
{
	unsigned int uint;
	int  db_flags;
	str  atmp;
	str  vtmp;
	int_str avp_attr;
	int_str avp_val;
	int flags;

	flags = dbp->a.opd;

	if (just_val_flags==-1)
	{
		/* check for null fields into the row */
		if (row->values[0].nul || row->values[1].nul || row->values[2].nul )
		{
			LOG( L_ERR, "ERROR:avpops:dbrow2avp: dbrow contains NULL fields\n");
			return -1;
		}

		/* check the value types */
		if ( (row->values[0].type!=DB_STRING && row->values[0].type!=DB_STR)
			||  (row->values[1].type!=DB_STRING && row->values[1].type!=DB_STR)
			|| row->values[2].type!=DB_INT )
		{
			LOG(L_ERR,"ERROR:avpops:dbrow2avp: wrong field types in dbrow\n");
			return -1;
		}

		/* check the content of flag field */
		uint = (unsigned int)row->values[2].val.int_val;
		db_flags = ((uint&AVPOPS_DB_NAME_INT)?0:AVP_NAME_STR) |
			((uint&AVPOPS_DB_VAL_INT)?0:AVP_VAL_STR);
	
		DBG("db_flags=%d, flags=%d\n",db_flags,flags);
		/* does the avp type match ? */
		if(!((flags&(AVPOPS_VAL_INT|AVPOPS_VAL_STR))==0 ||
				((flags&AVPOPS_VAL_INT)&&((db_flags&AVP_NAME_STR)==0)) ||
				((flags&AVPOPS_VAL_STR)&&(db_flags&AVP_NAME_STR))))
			return -2;
	} else {
		/* check the validity of value column */
		if (row->values[0].nul || (row->values[0].type!=DB_STRING &&
		row->values[0].type!=DB_STR && row->values[0].type!=DB_INT) )
		{
			LOG(L_ERR,"ERROR:avpops:dbrow2avp: empty or wrong type for"
				" 'value' using scheme\n");
			return -1;
		}
		db_flags = just_val_flags;
	}

	/* is the avp name already known? */
	if ( (flags&AVPOPS_VAL_NONE)==0 )
	{
		/* use the name  */
		avp_attr = attr;
	} else {
		/* take the name from db response */
		if (row->values[1].type==DB_STRING)
		{
			atmp.s = (char*)row->values[1].val.string_val;
			atmp.len = strlen(atmp.s);
		} else {
			atmp = row->values[1].val.str_val;
		}
		if (db_flags&AVP_NAME_STR)
		{
			/* name is string */
			avp_attr.s = atmp;
		} else {
			/* name is ID */
			if (str2int( &atmp, &uint)==-1)
			{
				LOG(L_ERR,"ERROR:avpops:dbrow2avp: name is not ID as "
					"flags say <%s>\n", atmp.s);
				return -1;
			}
			avp_attr.n = (int)uint;
		}
	}

	/* now get the value as correct type */
	if (row->values[0].type==DB_STRING)
	{
		vtmp.s = (char*)row->values[0].val.string_val;
		vtmp.len = strlen(vtmp.s);
	} else if (row->values[0].type==DB_STR){
		vtmp = row->values[0].val.str_val;
	} else {
		vtmp.s = 0;
		vtmp.len = 0;
	}
	if (db_flags&AVP_VAL_STR) {
		/* value must be saved as string */
		if (row->values[0].type==DB_INT) {
			vtmp.s = int2str( (unsigned long)row->values[0].val.int_val,
				&vtmp.len);
		}
		avp_val.s = vtmp;
	} else {
		/* value must be saved as integer */
		if (row->values[0].type!=DB_INT) {
			if (vtmp.len==0 || vtmp.s==0 || str2int(&vtmp, &uint)==-1) {
				LOG(L_ERR,"ERROR:avpops:dbrow2avp: value is not int "
					"as flags say <%s>\n", vtmp.s);
				return -1;
			}
			avp_val.n = (int)uint;
		} else {
			avp_val.n = row->values[0].val.int_val;
		}
	}

	/* added the avp */
	db_flags |= AVP_IS_IN_DB;
	/* set script flags */
	db_flags |= (dbp->a.sval.flags>>16)&0xff00;
	return add_avp( (unsigned short)db_flags, avp_attr, avp_val);
}


inline static str* get_source_uri(struct sip_msg* msg,int source)
{
	/* which uri will be used? */
	if (source&AVPOPS_USE_FROM)
	{ /* from */
		if (parse_from_header( msg )<0 )
		{
			LOG(L_ERR,"ERROR:avpops:get_source_uri: failed "
				"to parse from\n");
			goto error;
		}
		return &(get_from(msg)->uri);
	} else if (source&AVPOPS_USE_TO)
	{  /* to */
		if (parse_headers( msg, HDR_TO_F, 0)<0)
		{
			LOG(L_ERR,"ERROR:avpops:get_source_uri: failed "
				"to parse to\n");
			goto error;
		}
		return &(get_to(msg)->uri);
	} else if (source&AVPOPS_USE_RURI) {  /* RURI */
		if(msg->new_uri.s!=NULL && msg->new_uri.len>0)
			return &(msg->new_uri);
		return &(msg->first_line.u.request.uri);
	} else {
		LOG(L_CRIT,"BUG:avpops:get_source_uri: unknow source <%d>\n",
			source);
		goto error;
	}
error:
	return 0;
}

static inline void int_str2db_val( int_str is_val, str *val, int is_s)
{
	if (is_s)
	{
		/* val is string */
		*val = is_val.s;
	} else {
		/* val is integer */
		val->s =
			int2str((unsigned long)is_val.n, &val->len);
	}
}

static int avpops_get_aname(struct sip_msg* msg, struct fis_param *ap,
		int_str *avp_name, unsigned short *name_type)
{
	if(ap==NULL || avp_name==NULL || name_type==NULL)
	{
		LOG(L_ERR, "avpops:avpops_get_aname: bad parameters\n");
		return -1;
	}

	return xl_get_avp_name(msg, &ap->sval, avp_name, name_type);
}

#define AVPOPS_ATTR_LEN	64
static char avpops_attr_buf[AVPOPS_ATTR_LEN];

int ops_dbload_avps (struct sip_msg* msg, struct fis_param *sp,
									struct db_param *dbp, int use_domain)
{
	struct sip_uri   uri;
	db_res_t         *res = NULL;
	str              uuid;
	int  i, n, sh_flg;
	str *s0, *s1, *s2;
	int_str avp_name;
	xl_value_t xvalue;

	s0 = s1 = s2 = NULL;
	if (!((sp->opd&AVPOPS_VAL_PVAR)||(sp->opd&AVPOPS_VAL_STR))) {
		LOG(L_CRIT,"BUG:avpops:load_avps: invalid flag combination (%d/%d)\n",
			sp->opd, sp->ops);
		goto error;
	}

	/* get uuid from avp */
	if (sp->opd&AVPOPS_VAL_PVAR)
	{
		if(xl_get_spec_value(msg, &(sp->sval), &xvalue, 0)!=0)
		{
			LOG(L_CRIT,
				"BUG:avpops:load_avps: error getting PVAR value (%d/%d)\n",
				sp->opd, sp->ops);
			goto error;
		}
		if(xvalue.flags&(XL_VAL_NULL|XL_VAL_EMPTY))
		{
			LOG(L_ERR,
				"ERROR:avpops:load_avps: no value for first param\n");
			goto error;
		}
		uuid = xvalue.rs;
	} else {
		uuid.s   = sp->sval.p.val.s;
		uuid.len = sp->sval.p.val.len;
	}
	
	if(sp->opd&AVPOPS_FLAG_UUID0)
	{
		s0 = &uuid;
	} else {
		/* parse uri */
		if (parse_uri(uuid.s, uuid.len, &uri)<0)
		{
			LOG(L_ERR,"ERROR:avpops:load_avps: failed to parse uri\n");
			goto error;
		}

		/* check uri */
		if(!uri.user.s|| !uri.user.len|| !uri.host.len|| !uri.host.s)
		{
			LOG(L_ERR,"ERROR:avpops:load_avps: incomplet uri <%.*s>\n",
				uuid.len, uuid.s);
			goto error;
		}
		if((sp->opd&AVPOPS_FLAG_URI0)||(sp->opd&AVPOPS_FLAG_USER0))
			s1 = &uri.user;
		if((sp->opd&AVPOPS_FLAG_URI0)||(sp->opd&AVPOPS_FLAG_DOMAIN0))
			s2 = &uri.host;
	}

	/* is dynamic avp name ? */
	if(dbp->a.sval.flags&XL_DPARAM)
	{
		if(xl_get_spec_name(msg, &(dbp->a.sval), &xvalue, 0)!=0)
		{
			LOG(L_CRIT,
				"BUG:avpops:load_avps: error getting value for P2\n");
			goto error;
		}
		if(xvalue.flags&(XL_VAL_NULL|XL_VAL_EMPTY))
		{
			LOG(L_INFO, 
					"INFO:avpops:load_avps: no value for p2\n");
			goto error;
		}
		if(xvalue.flags&XL_VAL_STR)
		{
			if(xvalue.rs.len>=AVPOPS_ATTR_LEN)
			{
				LOG(L_ERR, 
					"ERROR:avpops:load_avps: name too long [%d/%.*s...]\n",
					xvalue.rs.len, 16, xvalue.rs.s);
				goto error;
			}
			dbp->sa.s = avpops_attr_buf;
			memcpy(dbp->sa.s, xvalue.rs.s, xvalue.rs.len);
			dbp->sa.len = xvalue.rs.len;
			dbp->sa.s[dbp->sa.len] = '\0';
		} else {
			LOG(L_INFO, 
					"INFO:avpops:load_avps: no string value for p2\n");
			goto error;
		}
	}

	/* do DB query */
	res = db_load_avp( s0, s1,
			((use_domain)||(sp->opd&AVPOPS_FLAG_DOMAIN0))?s2:0,
			dbp->sa.s, dbp->table, dbp->scheme);

	/* res query ?  */
	if (res==0)
	{
		LOG(L_ERR,"ERROR:avpops:load_avps: db_load failed\n");
		goto error;
	}

	sh_flg = (dbp->scheme)?dbp->scheme->db_flags:-1;
	/* process the results */
	for( n=0,i=0 ; i<res->n ; i++)
	{
		/* validate row */
		memset(&avp_name, 0, sizeof(int_str));
		if(dbp->a.sval.flags&XL_DPARAM)
		{
			if(xvalue.flags&XL_TYPE_INT)
			{
				avp_name.n = xvalue.ri;
			} else {
				avpops_str2int_str(xvalue.rs, avp_name);
			}
		} else {
			avpops_str2int_str(dbp->a.sval.p.val, avp_name);
		}
		//if ( dbrow2avp( &res->rows[i], dbp->a.opd, avp_name, sh_flg) < 0 )
		if ( dbrow2avp( &res->rows[i], dbp, avp_name, sh_flg) < 0 )
			continue;
		n++;
	}

	db_close_query( res );

	DBG("DEBUG:avpops:load_avps: loaded avps = %d\n",n);

	return n?1:-1;
error:
	return -1;
}


int ops_dbdelete_avps (struct sip_msg* msg, struct fis_param *sp,
									struct db_param *dbp, int use_domain)
{
	struct sip_uri  uri;
	int             res;
	str             uuid;
	xl_value_t xvalue;
	str *s0, *s1, *s2;

	s0 = s1 = s2 = NULL;
	if (!((sp->opd&AVPOPS_VAL_PVAR)||(sp->opd&AVPOPS_VAL_STR))) {
		LOG(L_CRIT,
			"BUG:avpops:dbdelete_avps: invalid flag combination (%d/%d)\n",
			sp->opd, sp->ops);
		goto error;
	}

	/* get uuid from avp */
	if (sp->opd&AVPOPS_VAL_PVAR)
	{
		if(xl_get_spec_value(msg, &(sp->sval), &xvalue, 0)!=0)
		{
			LOG(L_CRIT,
				"BUG:avpops:dbdelete_avps: error getting PVAR value (%d/%d)\n",
				sp->opd, sp->ops);
			goto error;
		}
		if(xvalue.flags&(XL_VAL_NULL|XL_VAL_EMPTY))
		{
			LOG(L_ERR,
				"ERROR:avpops:dbdelete_avps: no value for first param\n");
			goto error;
		}
		uuid = xvalue.rs;
	} else {
		uuid.s   = sp->sval.p.val.s;
		uuid.len = sp->sval.p.val.len;
	}
	
	if(sp->opd&AVPOPS_FLAG_UUID0)
	{
		s0 = &uuid;
	} else {
		/* parse uri */
		if (parse_uri(uuid.s, uuid.len, &uri)<0)
		{
			LOG(L_ERR,"ERROR:avpops:dbdelete_avps: failed to parse uri\n");
			goto error;
		}

		/* check uri */
		if(!uri.user.s|| !uri.user.len|| !uri.host.len|| !uri.host.s)
		{
			LOG(L_ERR,"ERROR:avpops:dbdelete_avps: incomplet uri <%.*s>\n",
				uuid.len, uuid.s);
			goto error;
		}
		if((sp->opd&AVPOPS_FLAG_URI0)||(sp->opd&AVPOPS_FLAG_USER0))
			s1 = &uri.user;
		if((sp->opd&AVPOPS_FLAG_URI0)||(sp->opd&AVPOPS_FLAG_DOMAIN0))
			s2 = &uri.host;
	}

	/* is dynamic avp name ? */
	if(dbp->a.sval.flags&XL_DPARAM)
	{
		if(xl_get_spec_name(msg, &(dbp->a.sval), &xvalue, 0)!=0)
		{
			LOG(L_CRIT,
				"BUG:avpops:dbdelete_avps: error getting value for P2\n");
			goto error;
		}
		if(xvalue.flags&(XL_VAL_NULL|XL_VAL_EMPTY))
		{
			LOG(L_INFO, 
					"INFO:avpops:dbdelete_avps: no value for p2\n");
			goto error;
		}
		if(xvalue.flags&XL_VAL_STR)
		{
			if(xvalue.rs.len>=AVPOPS_ATTR_LEN)
			{
				LOG(L_ERR, 
					"ERROR:avpops:dbdelete_avps: name too long [%d/%.*s...]\n",
					xvalue.rs.len, 16, xvalue.rs.s);
				goto error;
			}
			dbp->sa.s = avpops_attr_buf;
			memcpy(dbp->sa.s, xvalue.rs.s, xvalue.rs.len);
			dbp->sa.len = xvalue.rs.len;
			dbp->sa.s[dbp->sa.len] = '\0';
		} else {
			LOG(L_INFO, 
					"INFO:avpops:dbdelete_avps: no string value for p2\n");
			goto error;
		}
	}

	/* do DB delete */
	res = db_delete_avp(s0, s1,
			(use_domain||(sp->opd&AVPOPS_FLAG_DOMAIN0))?s2:0,
			dbp->sa.s, dbp->table);

	/* res ?  */
	if (res<0)
	{
		LOG(L_ERR,"ERROR:avpops:dbdelete_avps: db_delete failed\n");
		goto error;
	}

	return 1;
error:
	return -1;
}


int ops_dbstore_avps (struct sip_msg* msg, struct fis_param *sp,
									struct db_param *dbp, int use_domain)
{
	struct sip_uri   uri;
	struct usr_avp   **avp_list;
	struct usr_avp   *avp;
	unsigned short   name_type;
	int_str          avp_name;
	int_str          i_s;
	str              uuid;
	int              keys_nr;
	int              n;
	xl_value_t xvalue;
	str *s0, *s1, *s2;
	str *sn;

	s0 = s1 = s2 = NULL;
	name_type = 0;
	if (!((sp->opd&AVPOPS_VAL_PVAR)||(sp->opd&AVPOPS_VAL_STR))) {
		LOG(L_CRIT,
			"BUG:avpops:dbstore_avps: invalid flag combination (%d/%d)\n",
			sp->opd, sp->ops);
		goto error;
	}

	keys_nr = 6; /* uuid, avp name, avp val, avp type, user, domain */
	
	/* get uuid from avp */
	if (sp->opd&AVPOPS_VAL_PVAR)
	{
		if(xl_get_spec_value(msg, &(sp->sval), &xvalue, 0)!=0)
		{
			LOG(L_CRIT,
				"BUG:avpops:dbstore_avps: error getting PVAR value (%d/%d)\n",
				sp->opd, sp->ops);
			goto error;
		}
		if(xvalue.flags&(XL_VAL_NULL|XL_VAL_EMPTY))
		{
			LOG(L_ERR,
				"ERROR:avpops:dbstore_avps: no value for first param\n");
			goto error;
		}
		uuid = xvalue.rs;
	} else {
		uuid.s   = sp->sval.p.val.s;
		uuid.len = sp->sval.p.val.len;
	}
	
	if(sp->opd&AVPOPS_FLAG_UUID0)
	{
		s0 = &uuid;
	} else {
		/* parse uri */
		if (parse_uri(uuid.s, uuid.len, &uri)<0)
		{
			LOG(L_ERR,"ERROR:avpops:dbstore_avps: failed to parse uri\n");
			goto error;
		}

		/* check uri */
		if(!uri.user.s|| !uri.user.len|| !uri.host.len|| !uri.host.s)
		{
			LOG(L_ERR,"ERROR:avpops:dbstore_avps: incomplet uri <%.*s>\n",
				uuid.len, uuid.s);
			goto error;
		}
		if((sp->opd&AVPOPS_FLAG_URI0)||(sp->opd&AVPOPS_FLAG_USER0))
			s1 = &uri.user;
		if((sp->opd&AVPOPS_FLAG_URI0)||(sp->opd&AVPOPS_FLAG_DOMAIN0))
			s2 = &uri.host;
	}

	/* set values for keys  */
	store_vals[0].val.str_val = (s0)?*s0:empty;
	store_vals[4].val.str_val = (s1)?*s1:empty;
	if (use_domain || sp->opd&AVPOPS_FLAG_DOMAIN0)
		store_vals[5].val.str_val = (s2)?*s2:empty;

	/* is dynamic avp name ? */
	if(dbp->a.sval.flags&XL_DPARAM)
	{
		if(xl_get_spec_name(msg, &(dbp->a.sval), &xvalue, 0)!=0)
		{
			LOG(L_CRIT,
				"BUG:avpops:dbstore_avps: error getting value for P2\n");
			goto error;
		}
		if(xvalue.flags&(XL_VAL_NULL|XL_VAL_EMPTY))
		{
			LOG(L_INFO, 
					"INFO:dbstore_avps: no value for P2\n");
			goto error;
		}
		if(xvalue.flags&XL_TYPE_INT)
		{
			name_type = 0;
			avp_name.n = xvalue.ri;
		} else {
			name_type = AVP_NAME_STR;
		}
		if(xvalue.flags&XL_VAL_STR)
		{
			if(xvalue.rs.len>=AVPOPS_ATTR_LEN)
			{
				LOG(L_ERR, 
					"ERROR:avpops:dbstore_avps: name too long [%d/%.*s...]\n",
					xvalue.rs.len, 16, xvalue.rs.s);
				goto error;
			}
			dbp->sa.s = avpops_attr_buf;
			memcpy(dbp->sa.s, xvalue.rs.s, xvalue.rs.len);
			dbp->sa.len = xvalue.rs.len;
			dbp->sa.s[dbp->sa.len] = '\0';
			avp_name.s = dbp->sa;
		} else {
			LOG(L_INFO, 
					"INFO:avpops:dbstore_avps: no string value for p2\n");
			goto error;
		}
	} else if((dbp->a.opd&AVPOPS_VAL_NONE)==0) {
		name_type = (((dbp->a.opd&AVPOPS_VAL_INT))?0:AVP_NAME_STR);
		if(dbp->a.opd&AVPOPS_VAL_INT)
		{
			avp_name.n = dbp->a.sval.p.val.len;
		} else {
			avp_name.s = dbp->a.sval.p.val;
		}
	}

	/* set the script flags */
	name_type |= ((dbp->a.sval.flags>>16)&0xff00);
	
	/* set uuid/(username and domain) fields */

	n =0 ;

	if ((dbp->a.opd&AVPOPS_VAL_NONE)==0)
	{
		/* avp name is known ->set it and its type */
		store_vals[1].val.str_val = dbp->sa; /*attr name*/
		avp = search_first_avp( name_type, avp_name, &i_s, 0);
		for( ; avp; avp=search_first_avp( name_type, avp_name, &i_s, avp))
		{
			/* don't insert avps which were loaded */
			if (avp->flags&AVP_IS_IN_DB)
				continue;
			/* set type */
			store_vals[3].val.int_val =
				(avp->flags&AVP_NAME_STR?0:AVPOPS_DB_NAME_INT)|
				(avp->flags&AVP_VAL_STR?0:AVPOPS_DB_VAL_INT);
			/* set value */
			int_str2db_val( i_s, &store_vals[2].val.str_val,
				avp->flags&AVP_VAL_STR);
			/* save avp */
			if (db_store_avp( store_keys, store_vals,
					keys_nr, dbp->table)==0 )
			{
				avp->flags |= AVP_IS_IN_DB;
				n++;
			}
		}
	} else {
		/* avp name is unknown -> go through all list */
		avp_list = get_avp_list();
		avp = *avp_list;

		for ( ; avp ; avp=avp->next )
		{
			/* don't insert avps which were loaded */
			if (avp->flags&AVP_IS_IN_DB)
				continue;
			/* check if type match */
			if ( !( (dbp->a.opd&(AVPOPS_VAL_INT|AVPOPS_VAL_STR))==0 ||
				((dbp->a.opd&AVPOPS_VAL_INT)&&((avp->flags&AVP_NAME_STR))==0)
				||((dbp->a.opd&AVPOPS_VAL_STR)&&(avp->flags&AVP_NAME_STR))))
				continue;

			/* set attribute name and type */
			if ( (sn=get_avp_name(avp))==0 )
				i_s.n = avp->id;
			else
				i_s.s = *sn;
			int_str2db_val( i_s, &store_vals[1].val.str_val,
				avp->flags&AVP_NAME_STR);
			store_vals[3].val.int_val =
				(avp->flags&AVP_NAME_STR?0:AVPOPS_DB_NAME_INT)|
				(avp->flags&AVP_VAL_STR?0:AVPOPS_DB_VAL_INT);
			/* set avp value */
			get_avp_val( avp, &i_s);
			int_str2db_val( i_s, &store_vals[2].val.str_val,
				avp->flags&AVP_VAL_STR);
			/* save avp */
			if (db_store_avp( store_keys, store_vals,
			keys_nr, dbp->table)==0)
			{
				avp->flags |= AVP_IS_IN_DB;
				n++;
			}
		}
	}

	DBG("DEBUG:avpops:dbstore_avps: %d avps were stored\n",n);

	return n==0?-1:1;
error:
	return -1;
}

int ops_dbquery_avps(struct sip_msg* msg, xl_elem_t* query,
		itemname_list_t* dest)
{
	int printbuf_len;

	if(msg==NULL || query==NULL)
	{
		LOG(L_ERR,"ERROR:avpops:ops_dbquery_avps: bad parameters\n");
		return -1;
	}
	
	printbuf_len = AVP_PRINTBUF_SIZE-1;
	if(xl_printf(msg, query, printbuf, &printbuf_len)<0 || printbuf_len<=0)
	{
		LOG(L_ERR,"avpops:ops_dbquery_avps: error - cannot print the query\n");
		return -1;
	}

	DBG("avpops:ops_dbquery_avps: query [%s]\n", printbuf);
	
	if(db_query_avp(msg, printbuf, dest)!=0)
		return -1;
	return 1;
}

int ops_write_avp(struct sip_msg* msg, struct fis_param *src,
													struct fis_param *dst)
{
	struct sip_uri uri;
	int_str avp_val;
	int_str avp_name;
	unsigned short flags;
	xl_value_t xvalue;
	unsigned short name_type;
	
	flags = 0;
	if (src->opd&AVPOPS_VAL_PVAR)
	{
		if(xl_get_spec_value(msg, &(src->sval), &xvalue, 0)!=0)
		{
			LOG(L_ERR,"ERROR:avpops:write_avp: cannot get value\n");
			goto error;
		}
		if(xvalue.flags&XL_VAL_NULL)
			return -1;
		if(xvalue.flags&XL_TYPE_INT)
		{
			avp_val.n = xvalue.ri;
		} else {
			flags = AVP_VAL_STR;
			avp_val.s = xvalue.rs;
		}
	} else {
		avpops_str2int_str(src->sval.p.val, avp_val);
		if(src->sval.p.val.s!=NULL)
			flags = AVP_VAL_STR;
	}
	/* check flags */
	if ((flags&AVP_VAL_STR)
			&&(src->opd&(AVPOPS_FLAG_USER0|AVPOPS_FLAG_DOMAIN0)))
	{
		if (parse_uri(avp_val.s.s, avp_val.s.len, &uri)!=0 )
		{
				LOG(L_ERR,"ERROR:avpops:write_avp: cannot parse uri\n");
				goto error;
		}
		if (src->opd&AVPOPS_FLAG_DOMAIN0)
			avp_val.s = uri.host;
		else {
			if(uri.user.len<=0)
				return -1;
			avp_val.s = uri.user;
		}
	}

	/* get dst avp name */
	if(avpops_get_aname(msg, dst, &avp_name, &name_type)!=0)
	{
		LOG(L_ERR,
			"avpops:write_avp: error getting dst AVP name\n");
		goto error;
	}

	flags |=  name_type;

	/* added the avp */
	if (add_avp(flags, avp_name, avp_val)<0)
		goto error;

	return 1;
error:
	return -1;
}


int ops_delete_avp(struct sip_msg* msg, struct fis_param *ap)
{
	struct usr_avp **avp_list;
	struct usr_avp *avp;
	struct usr_avp *avp_next;
	unsigned short name_type;
	int_str avp_name;
	int n;

	n = 0;

	if ( (ap->opd&AVPOPS_VAL_NONE)==0)
	{
		/* avp name is known ->search by name */
		/* get avp name */
		if(avpops_get_aname(msg, ap, &avp_name, &name_type)!=0)
		{
			LOG(L_ERR,
				"avpops:write_avp: error getting dst AVP name\n");
			return -1;
		}
		n = destroy_avps( name_type, avp_name, ap->ops&AVPOPS_FLAG_ALL );
	} else {
		/* avp name is not given - we have just flags */
		/* -> go through all list */
		avp_list = get_avp_list();
		avp = *avp_list;

		for ( ; avp ; avp=avp_next )
		{
			avp_next = avp->next;
			/* check if type match */
			if ( !( (ap->opd&(AVPOPS_VAL_INT|AVPOPS_VAL_STR))==0 ||
			((ap->opd&AVPOPS_VAL_INT)&&((avp->flags&AVP_NAME_STR))==0) ||
			((ap->opd&AVPOPS_VAL_STR)&&(avp->flags&AVP_NAME_STR)) )  )
				continue;
			if(((ap->sval.flags>>16)&AVP_SCRIPT_MASK)!=0
					&& (((ap->sval.flags>>16)&AVP_SCRIPT_MASK)&avp->flags)==0)
				continue;
			/* remove avp */
			destroy_avp( avp );
			n++;
			if ( !(ap->ops&AVPOPS_FLAG_ALL) )
				break;
		}
	}

	DBG("DEBUG:avpops:delete_avps: %d avps were removed\n",n);

	return n?1:-1;
}

int ops_copy_avp( struct sip_msg* msg, struct fis_param* src,
													struct fis_param* dst)
{
	struct usr_avp *avp;
	struct usr_avp *prev_avp;
	int_str         avp_val;
	int_str         avp_val2;
	unsigned short name_type1;
	unsigned short name_type2;
	int_str avp_name1;
	int_str avp_name2;
	int n;

	n = 0;
	prev_avp = 0;

	/* get avp src name */
	if(avpops_get_aname(msg, src, &avp_name1, &name_type1)!=0)
	{
		LOG(L_ERR,
			"avpops:copy_avp: error getting src AVP name\n");
		goto error;
	}
	/* get avp dst name */
	if(avpops_get_aname(msg, dst, &avp_name2, &name_type2)!=0)
	{
		LOG(L_ERR,
			"avpops:copy_avp: error getting dst AVP name\n");
		goto error;
	}

	avp = search_first_avp( name_type1, avp_name1, &avp_val, 0);
	while ( avp )
	{
		/* build a new avp with new name, but old value */
		/* do we need cast conversion ?!?! */
		if((avp->flags&AVP_VAL_STR) && (dst->ops&AVPOPS_FLAG_CASTN)) {
			if(str2int(&avp_val.s, (unsigned int*)&avp_val2.n)!=0)
			{
				LOG(L_ERR,"ERROR:avpops:copy_avp: cannot convert str to int\n");
				goto error;
			}
			if ( add_avp( name_type2, avp_name2, avp_val2)==-1 ) {
				LOG(L_ERR,"ERROR:avpops:copy_avp: failed to create new avp!\n");
				goto error;
			}
		} else if(!(avp->flags&AVP_VAL_STR)&&(dst->ops&AVPOPS_FLAG_CASTS)) {
			avp_val2.s.s = int2str(avp_val.n, &avp_val2.s.len);
			if ( add_avp( name_type2|AVP_VAL_STR, avp_name2, avp_val2)==-1 ) {
				LOG(L_ERR,"ERROR:avpops:copy_avp: failed to create new avp.\n");
				goto error;
			}
		} else {
			if ( add_avp( name_type2|(avp->flags&AVP_VAL_STR), avp_name2,
					avp_val)==-1 ) {
				LOG(L_ERR,"ERROR:avpops:copy_avp: failed to create new avp\n");
				goto error;
			}
		}
		n++;
		/* copy all avps? */
		if ( !(dst->ops&AVPOPS_FLAG_ALL) ) {
			/* delete the old one? */
			if (dst->ops&AVPOPS_FLAG_DELETE)
				destroy_avp( avp );
			break;
		} else {
			prev_avp = avp;
			avp = search_first_avp( name_type1, avp_name1, &avp_val, prev_avp);
			/* delete the old one? */
			if (dst->ops&AVPOPS_FLAG_DELETE)
				destroy_avp( prev_avp );
		}
	}

	return n?1:-1;
error:
	return -1;
}


#define STR_BUF_SIZE  1024
static char str_buf[STR_BUF_SIZE];

inline static int append_0(str *in, str *out)
{
	if (in->len+1>STR_BUF_SIZE)
		return -1;
	memcpy( str_buf, in->s, in->len);
	str_buf[in->len] = 0;
	out->len = in->len;
	out->s = str_buf;
	return 0;
}


int ops_pushto_avp (struct sip_msg* msg, struct fis_param* dst,
													struct fis_param* src)
{
	struct action  act;
	struct usr_avp *avp;
	unsigned short name_type;
	int_str        avp_val;
	int_str        avp_name;
	str            val;
	int            act_type;
	int            n;
	int            flags;
	xl_value_t     xvalue;

	avp = NULL;
	flags = 0;
	if(src->sval.type==XL_AVP)
	{
		/* search for the avp */
		if(avpops_get_aname(msg, src, &avp_name, &name_type)!=0)
		{
			LOG(L_ERR,
				"avpops:pushto_avp: error getting src AVP name\n");
			goto error;
		}
		avp = search_first_avp( name_type, avp_name, &avp_val, 0);
		if (avp==0)
		{
			DBG("DEBUG:avpops:pushto_avp: no src avp found\n");
			goto error;
		}
		flags = avp->flags;
	} else {
		if(xl_get_spec_value(msg, &(src->sval), &xvalue, 0)!=0)
		{
			LOG(L_ERR,"ERROR:avpops:pushto_avp: cannot get src value\n");
			goto error;
		}
		if(xvalue.flags&XL_TYPE_INT)
		{
			avp_val.n = xvalue.ri;
		} else {
			flags = AVP_VAL_STR;
			avp_val.s = xvalue.rs;
		}
	}

	n = 0;
	do {
		/* the avp val will be used all the time as str */
		if (flags&AVP_VAL_STR) {
			val = avp_val.s;
		} else {
			val.s = int2str((unsigned long)avp_val.n, &val.len);
		}

		act_type = 0;
		/* push the value into right position */
		if (dst->opd&AVPOPS_USE_RURI)
		{
			if (dst->opd&AVPOPS_FLAG_USER0)
				act_type = SET_USER_T;
			else if (dst->opd&AVPOPS_FLAG_DOMAIN0)
				act_type = SET_HOST_T;
			else
				act_type = SET_URI_T;
			if ( flags&AVP_VAL_STR && append_0( &val, &val)!=0 ) {
				LOG(L_ERR,"ERROR:avpops:pushto_avp: failed to make 0 term.\n");
				goto error;
			}
		} else if (dst->opd&AVPOPS_USE_DURI) {
			if (!(flags&AVP_VAL_STR)) {
				goto error;
			}
		} else if (dst->opd&AVPOPS_USE_BRANCH) {
			if (!(flags&AVP_VAL_STR)) {
				goto error;
			}
		} else {
			LOG(L_CRIT,"BUG:avpops:pushto_avp: destination unknown (%d/%d)\n",
				dst->opd, dst->ops);
			goto error;
		}
	
		if ( act_type )
		{
			/* rewrite part of ruri */
			if (n)
			{
				/* if is not the first modification, push the current uri as
				 * branch */
				if (append_branch( msg, 0, 0, 0, Q_UNSPECIFIED, 0, 0)!=1 )
				{
					LOG(L_ERR,"ERROR:avpops:pushto_avp: append_branch action"
						" failed\n");
					goto error;
				}
			}
			memset(&act, 0, sizeof(act));
			act.elem[0].type = STRING_ST;
			act.elem[0].u.string = val.s;
			act.type = act_type;
			if (do_action(&act, msg)<0)
			{
				LOG(L_ERR,"ERROR:avpops:pushto_avp: SET_XXXX_T action"
					" failed\n");
				goto error;
			}
		} else if (dst->opd&AVPOPS_USE_DURI) {
			if(set_dst_uri(msg, &val)!=0)
			{
				LOG(L_ERR,"ERROR:avpops:pushto_avp: changing dst uri "
					"failed\n");
				goto error;
			}
		} else if (dst->opd&AVPOPS_USE_BRANCH) {
			if (append_branch( msg, &val, 0, 0, Q_UNSPECIFIED, 0,
			msg->force_send_socket)!=1 )
			{
				LOG(L_ERR,"ERROR:avpops:pushto_avp: append_branch action"
					" failed\n");
				goto error;
			}
		} else {
			LOG(L_ERR, "ERROR:avpops:pushto_avp: unknown destination\n");
			goto error;
		}

		n++;
		if ( !(src->ops&AVPOPS_FLAG_ALL) )
			break;
		if(avp==NULL)
			break;
		if((avp = search_first_avp( name_type, avp_name, &avp_val, avp))!=NULL)
			flags = avp->flags;
	} while (avp);/* end while */

	DBG("DEBUG:avpops:pushto_avps: %d avps were processed\n",n);
	return 1;
error:
	return -1;
}

int ops_check_avp( struct sip_msg* msg, struct fis_param* src,
													struct fis_param* val)
{
	unsigned short    name_type1;
	unsigned short    name_type2;
	struct usr_avp    *avp1;
	struct usr_avp    *avp2;
	regmatch_t        pmatch;
	int_str           avp_name1;
	int_str           avp_name2;
	int_str           avp_val;
	int_str           check_val;
	int               check_flags;
	int               n, rt;
	int            flags;
	xl_value_t     xvalue;
	char           backup;

	/* look if the required avp(s) is/are present */
	if(src->sval.type==XL_AVP)
	{
		/* search for the avp */
		if(avpops_get_aname(msg, src, &avp_name1, &name_type1)!=0)
		{
			LOG(L_ERR,
				"avpops:ops_check_avp: error getting src AVP name\n");
			goto error;
		}
		avp1 = search_first_avp( name_type1, avp_name1, &avp_val, 0);
		if (avp1==0)
		{
			DBG("DEBUG:avpops:ops_check_avp: no src avp found\n");
			goto error;
		}
		flags = avp1->flags;
	} else {
		avp1 = 0;
		flags = 0;
		if(xl_get_spec_value(msg, &(src->sval), &xvalue, 0)!=0)
		{
			LOG(L_ERR,"ERROR:avpops:ops_check_avp: cannot get src value\n");
			goto error;
		}
		if(xvalue.flags&XL_TYPE_INT)
		{
			avp_val.n = xvalue.ri;
		} else {
			flags = AVP_VAL_STR;
			avp_val.s = xvalue.rs;
		}
	}

cycle1:
	/* copy string since pseudo-variables uses static buffer */
	if(flags&AVP_VAL_STR)
	{
		if(avp_val.s.len>=STR_BUF_SIZE)
		{
			LOG(L_ERR,
				"avpops:ops_check_avp: error src value too long\n");
			goto error;
		}
		strncpy(str_buf, avp_val.s.s, avp_val.s.len);
		str_buf[avp_val.s.len] = '\0';
		avp_val.s.s = str_buf;
	}

	if (val->opd&AVPOPS_VAL_PVAR)
	{
		/* the 2nd operator is variable -> get avp value */
		check_flags = 0;
		if(val->sval.type==XL_AVP)
		{
			/* search for the avp */
			if(avpops_get_aname(msg, val, &avp_name2, &name_type2)!=0)
			{
				LOG(L_ERR,
					"avpops:ops_check_avp: error getting dst AVP name\n");
				goto error;
			}
			avp2 = search_first_avp( name_type2, avp_name2, &check_val, 0);
			if (avp2==0)
			{
				DBG("DEBUG:avpops:ops_check_avp: no dst avp found\n");
				goto error;
			}
			check_flags = avp2->flags;
		} else {
			avp2 = 0;
			if(xl_get_spec_value(msg, &(val->sval), &xvalue, 0)!=0)
			{
				LOG(L_ERR,"ERROR:avpops:ops_check_avp: cannot get dst value\n");
				goto error;
			}
			if(xvalue.flags&XL_TYPE_INT)
			{
				check_val.n = xvalue.ri;
			} else {
				check_flags = AVP_VAL_STR;
				check_val.s = xvalue.rs;
			}
		}
	} else {
		check_flags = 0;
		avpops_str2int_str(val->sval.p.val, check_val);
		if(val->sval.p.val.s)
			check_flags = AVP_VAL_STR;
		avp2 = 0;
	}

cycle2:
	/* are both values of the same type? */
	if ((flags&AVP_VAL_STR)^(check_flags&AVP_VAL_STR))
	{
		LOG(L_ERR,"ERROR:avpops:check_avp: value types don't match\n");
		goto next;
	}

	if (flags&AVP_VAL_STR)
	{
		/* string values to check */
		DBG("DEBUG:avpops:check_avp: check <%.*s> against <%.*s> as str /%d\n",
			avp_val.s.len,avp_val.s.s,
			(val->ops&AVPOPS_OP_RE)?6:check_val.s.len,
			(val->ops&AVPOPS_OP_RE)?"REGEXP":check_val.s.s,
			val->ops);
		/* do check */
		if (val->ops&AVPOPS_OP_EQ)
		{
			if (avp_val.s.len==check_val.s.len)
			{
				if (val->ops&AVPOPS_FLAG_CI)
				{
					if (strncasecmp(avp_val.s.s,check_val.s.s,
								check_val.s.len)==0)
						return 1;
				} else {
					if (strncmp(avp_val.s.s,check_val.s.s,check_val.s.len)==0 )
						return 1;
				}
			}
		} else if (val->ops&AVPOPS_OP_NE) {
			if (avp_val.s.len!=check_val.s.len)
				return 1;
			if (val->ops&AVPOPS_FLAG_CI)
			{
				if (strncasecmp(avp_val.s.s,check_val.s.s,check_val.s.len)!=0)
					return 1;
			} else {
				if (strncmp(avp_val.s.s,check_val.s.s,check_val.s.len)!=0 )
					return 1;
			}
		} else if (val->ops&AVPOPS_OP_LT) {
			n = (avp_val.s.len>=check_val.s.len)?avp_val.s.len:check_val.s.len;
			rt = strncasecmp(avp_val.s.s,check_val.s.s,n);
			if (rt<0)
				return 1;
			if(rt==0 && avp_val.s.len<check_val.s.len)
				return 1;
		} else if (val->ops&AVPOPS_OP_LE) {
			n = (avp_val.s.len>=check_val.s.len)?avp_val.s.len:check_val.s.len;
			if (strncasecmp(avp_val.s.s,check_val.s.s,n)<=0)
				return 1;
		} else if (val->ops&AVPOPS_OP_GT) {
			n = (avp_val.s.len>=check_val.s.len)?avp_val.s.len:check_val.s.len;
			rt = strncasecmp(avp_val.s.s,check_val.s.s,n);
			if (rt>0)
				return 1;
			if(rt==0 && avp_val.s.len>check_val.s.len)
				return 1;
		} else if (val->ops&AVPOPS_OP_GE) {
			n = (avp_val.s.len>=check_val.s.len)?avp_val.s.len:check_val.s.len;
			if (strncasecmp(avp_val.s.s,check_val.s.s,n)>=0)
				return 1;
		} else if (val->ops&AVPOPS_OP_RE) {
			backup  = avp_val.s.s[avp_val.s.len];
			avp_val.s.s[avp_val.s.len] = '\0';
			if (regexec((regex_t*)check_val.s.s, avp_val.s.s, 1, &pmatch, 0)==0)
			{
				avp_val.s.s[avp_val.s.len] = backup;
				return 1;
			}
			avp_val.s.s[avp_val.s.len] = backup;
		} else if (val->ops&AVPOPS_OP_FM){
			backup  = avp_val.s.s[avp_val.s.len];
			avp_val.s.s[avp_val.s.len] = '\0';
			if (fnmatch( check_val.s.s, avp_val.s.s, 0)==0)
			{
				avp_val.s.s[avp_val.s.len] = backup;
				return 1;
			}
			avp_val.s.s[avp_val.s.len] = backup;
		} else {
			LOG(L_CRIT,"BUG:avpops:check_avp: unknown operation "
				"(flg=%d/%d)\n",val->opd, val->ops);
		}
	} else {
		/* int values to check -> do check */
		DBG("DEBUG:avpops:check_avp: check <%d> against <%d> as int /%d\n",
				avp_val.n, check_val.n, val->ops);
		if (val->ops&AVPOPS_OP_EQ)
		{
			if ( avp_val.n==check_val.n)
				return 1;
		} else 	if (val->ops&AVPOPS_OP_NE) {
			if ( avp_val.n!=check_val.n)
				return 1;
		} else  if (val->ops&AVPOPS_OP_LT) {
			if ( avp_val.n<check_val.n)
				return 1;
		} else if (val->ops&AVPOPS_OP_LE) {
			if ( avp_val.n<=check_val.n)
				return 1;
		} else if (val->ops&AVPOPS_OP_GT) {
			if ( avp_val.n>check_val.n)
				return 1;
		} else if (val->ops&AVPOPS_OP_GE) {
			if ( avp_val.n>=check_val.n)
				return 1;
		} else if (val->ops&AVPOPS_OP_BAND) {
			if ( avp_val.n&check_val.n)
				return 1;
		} else if (val->ops&AVPOPS_OP_BOR) {
			if ( avp_val.n|check_val.n)
				return 1;
		} else if (val->ops&AVPOPS_OP_BXOR) {
			if ( avp_val.n^check_val.n)
				return 1;
		} else {
			LOG(L_CRIT,"BUG:avpops:check_avp: unknown operation "
				"(flg=%d)\n",val->ops);
		}
	}

next:
	/* cycle for the second value (only if avp can have multiple vals) */
	if ((avp2!=NULL)
		&& (avp2=search_first_avp( name_type2, avp_name2, &check_val, avp2))!=NULL)
	{
		check_flags = avp2->flags;
		goto cycle2;
	/* cycle for the first value -> next avp */
	} else {
		if(avp1 && val->ops&AVPOPS_FLAG_ALL)
		{
			avp1=search_first_avp(name_type1, avp_name1, &avp_val, avp1);
			if (avp1)
				goto cycle1;
		}
	}

	DBG("DEBUG:avpops:check_avp: no match\n");
	return -1; /* check failed */
error:
	return -1;
}


int ops_print_avp()
{
	struct usr_avp **avp_list;
	struct usr_avp *avp;
	int_str         val;
	str            *name;

	/* go through all list */
	avp_list = get_avp_list();
	avp = *avp_list;

	for ( ; avp ; avp=avp->next)
	{
		LOG(L_INFO,"INFO:avpops:print_avp: p=%p, flags=0x%04X\n",avp,
				avp->flags);
		if (avp->flags&AVP_NAME_STR)
		{
			name = get_avp_name(avp);
			LOG(L_INFO,"INFO:\t\t\tname=<%.*s>\n",name->len,name->s);
		} else {
			LOG(L_INFO,"INFO:\t\t\tid=<%d>\n",avp->id);
		}
		get_avp_val( avp, &val);
		if (avp->flags&AVP_VAL_STR)
		{
			LOG(L_INFO,"INFO:\t\t\tval_str=<%.*s / %d>\n",val.s.len,val.s.s,
					val.s.len);
		} else {
			LOG(L_INFO,"INFO:\t\t\tval_int=<%d>\n",val.n);
		}
	}

	
	return 1;
}

int ops_printf(struct sip_msg* msg, struct fis_param* dest, xl_elem_t *format)
{
	int printbuf_len;
	int_str avp_val;
	int_str avp_name;
	unsigned short flags;
	unsigned short name_type;

	if(msg==NULL || dest==NULL || format==NULL)
	{
		LOG(L_ERR, "avpops:ops_printf: error - bad parameters\n");
		return -1;
	}
	
	printbuf_len = AVP_PRINTBUF_SIZE-1;
	if(xl_printf(msg, format, printbuf, &printbuf_len)<0)
	{
		LOG(L_ERR, "avpops:ops_printf: error - cannot print the format\n");
		return -1;
	}
	avp_val.s.s   = printbuf;
	avp_val.s.len = printbuf_len;
		
	/* set the proper flag */
	flags = AVP_VAL_STR;
	
	/* is dynamic avp name ? */
	if(avpops_get_aname(msg, dest, &avp_name, &name_type)!=0)
	{
		LOG(L_ERR,
			"BUG:avpops:ops_printf: error getting dst AVP name\n");
		return -1;
	}

	if(name_type == AVP_NAME_STR)
		flags |=  AVP_NAME_STR;

	if (add_avp(flags, avp_name, avp_val)<0)
	{
		LOG(L_ERR, "avpops:ops_printf: error - cannot add AVP\n");
		return -1;
	}

	return 1;
}

int ops_subst(struct sip_msg* msg, struct fis_param** src,
		struct subst_expr* se)
{
	struct usr_avp *avp;
	struct usr_avp *prev_avp;
	int_str         avp_val;
	unsigned short name_type1;
	unsigned short name_type2;
	int_str         avp_name1;
	int_str         avp_name2;
	int n;
	int nmatches;
	str* result;

	n = 0;
	prev_avp = 0;

	/* avp name is known ->search by name */
	/* get src avp name */
	if(avpops_get_aname(msg, src[0], &avp_name1, &name_type1)!=0)
	{
		LOG(L_ERR,
			"BUG:avpops:ops_subst: error getting src AVP name\n");
		return -1;
	}

	avp = search_first_avp(name_type1, avp_name1, &avp_val, 0);

	if(avp==NULL)
		return -1;
	
	if(src[1]!=0)
	{
		/* get dst avp name */
		if(avpops_get_aname(msg, src[1], &avp_name2, &name_type2)!=0)
		{
			LOG(L_ERR,
				"BUG:avpops:ops_subst: error getting dst AVP name\n");
			return -1;
		}
	} else {
		name_type2 = name_type1;
		avp_name2 = avp_name1;
	}
	
	if(name_type2&AVP_NAME_STR)
	{
		if(avp_name2.s.len>=STR_BUF_SIZE)
		{
			LOG(L_ERR,
				"avpops:ops_subst: error dst name too long\n");
			goto error;
		}
		strncpy(str_buf, avp_name2.s.s, avp_name2.s.len);
		str_buf[avp_name2.s.len] = '\0';
		avp_name2.s.s = str_buf;
	}

	while(avp)
	{
		if(!is_avp_str_val(avp))
		{
			prev_avp = avp;
			avp = search_first_avp(name_type1, avp_name1, &avp_val, prev_avp);
			continue;
		}
		
		result=subst_str(avp_val.s.s, msg, se, &nmatches);
		if(result!=NULL)
		{
			/* build a new avp with new name */
			avp_val.s = *result;
			if(add_avp(name_type2|AVP_VAL_STR, avp_name2, avp_val)==-1 ) {
				LOG(L_ERR,"ERROR:avpops:ops_subst: failed to create new avp\n");
				if(result->s!=0)
					pkg_free(result->s);
				pkg_free(result);
				goto error;
			}
			if(result->s!=0)
				pkg_free(result->s);
			pkg_free(result);
			n++;
			/* copy all avps? */
			if (!(src[0]->ops&AVPOPS_FLAG_ALL) ) {
				/* delete the old one? */
				if (src[0]->ops&AVPOPS_FLAG_DELETE || src[1]==0)
					destroy_avp(avp);
				break;
			} else {
				prev_avp = avp;
				avp = search_first_avp(name_type1,avp_name1,&avp_val,prev_avp);
				/* delete the old one? */
				if (src[0]->ops&AVPOPS_FLAG_DELETE || src[1]==0)
					destroy_avp( prev_avp );
			}
		} else {
			prev_avp = avp;
			avp = search_first_avp(name_type1, avp_name1, &avp_val, prev_avp);
		}

	}
	DBG("avpops:ops_subst: subst to %d avps\n", n);
	return n?1:-1;
error:
	return -1;
}

int ops_op_avp( struct sip_msg* msg, struct fis_param** av,
													struct fis_param* val)
{
	unsigned short    name_type1;
	unsigned short    name_type2;
	unsigned short    name_type3;
	struct fis_param* src;
	struct usr_avp    *avp1;
	struct usr_avp    *avp2;
	struct usr_avp    *prev_avp;
	int_str           avp_name1;
	int_str           avp_name2;
	int_str           avp_name3;
	int_str           avp_val;
	int_str           op_val;
	int               op_flags;
	int               result;
	xl_value_t        xvalue;

	src = av[0];
	/* look if the required avp(s) is/are present */
			/* search for the avp */
	if(avpops_get_aname(msg, src, &avp_name1, &name_type1)!=0)
	{
		LOG(L_ERR,
			"avpops:ops_op_avp: error getting src AVP name\n");
		goto error;
	}
	avp1 = search_first_avp(name_type1, avp_name1, &avp_val, 0);
	if (avp1==0)
	{
		DBG("DEBUG:avpops:ops_op_avp: no src avp found\n");
		goto error;
	}

	while(avp1!=0)
	{
		if(!(avp1->flags&AVP_VAL_STR))
			break;
		avp1 = search_first_avp(name_type1, avp_name1, &avp_val, avp1);
	}
	if (avp1==0 && !(val->ops&AVPOPS_OP_BNOT)) {
		DBG("DEBUG:avpops:op_avp: no proper avp found\n");
		goto error;
	}
	name_type3 = name_type1;
	avp_name3 = avp_name1;
	if(av[1]!=0)
	{
		if(avpops_get_aname(msg, av[1], &avp_name3, &name_type3)!=0)
		{
			LOG(L_ERR,
				"avpops:ops_op_avp: error getting dst AVP name\n");
			goto error;
		}
	}
	if(name_type3&AVP_NAME_STR)
	{
		if(avp_name3.s.len>=STR_BUF_SIZE)
		{
			LOG(L_ERR,
				"avpops:ops_op_avp: error dst name too long\n");
			goto error;
		}
		strncpy(str_buf, avp_name3.s.s, avp_name3.s.len);
		str_buf[avp_name3.s.len] = '\0';
		avp_name3.s.s = str_buf;
	}
	prev_avp = 0;
	result = 0;

cycle1:
	if (val->opd&AVPOPS_VAL_PVAR)
	{
		/* the 2nd operator is variable -> get value */
		op_flags = 0;
		if(val->sval.type==XL_AVP)
		{
			/* search for the avp */
			if(avpops_get_aname(msg, val, &avp_name2, &name_type2)!=0)
			{
				LOG(L_ERR,
					"avpops:ops_op_avp: error getting dst AVP name\n");
				goto error;
			}
			avp2 = search_first_avp( name_type2, avp_name2, &op_val, 0);
			while(avp2!=0)
			{
				if(!(avp2->flags&AVP_VAL_STR))
					break;
				avp2 = search_first_avp( name_type2, avp_name2, &op_val, avp2);
			}
			if (avp2==0)
			{
				DBG("DEBUG:avpops:op_avp: no dst avp found\n");
				goto error;
			}
		} else {
			avp2 = 0;
			if(xl_get_spec_value(msg, &(val->sval), &xvalue, 0)!=0)
			{
				LOG(L_ERR,"ERROR:avpops:ops_op_avp: cannot get dst value\n");
				goto error;
			}
			if(xvalue.flags&XL_TYPE_INT)
			{
				op_val.n = xvalue.ri;
			} else {
				LOG(L_ERR,
					"ERROR:avpops:ops_op_avp: error - dst value is str\n");
				goto error;
			}
		}
	} else {
		avpops_str2int_str(val->sval.p.val, op_val);
		avp2 = 0;
	}

cycle2:
	/* do operation */
	DBG("DEBUG:avpops:op_avp: use <%d> and <%d>\n",
			avp_val.n, op_val.n);
	if (val->ops&AVPOPS_OP_ADD)
	{
		result = avp_val.n+op_val.n;
	} else 	if (val->ops&AVPOPS_OP_SUB) {
		result = avp_val.n-op_val.n;
	} else  if (val->ops&AVPOPS_OP_MUL) {
		result = avp_val.n*op_val.n;
	} else if (val->ops&AVPOPS_OP_DIV) {
		if(op_val.n!=0)
			result = (int)(avp_val.n/op_val.n);
		else
		{
			LOG(L_ERR, "avpops:op_avp: error - division by 0\n");
			result = 0;
		}
	} else if (val->ops&AVPOPS_OP_MOD) {
		if(op_val.n!=0)
			result = avp_val.n%op_val.n;
		else
		{
			LOG(L_ERR, "avpops:op_avp: error - modulo by 0\n");
			result = 0;
		}
	} else if (val->ops&AVPOPS_OP_BAND) {
		result = avp_val.n&op_val.n;
	} else if (val->ops&AVPOPS_OP_BOR) {
		result = avp_val.n|op_val.n;
	} else if (val->ops&AVPOPS_OP_BXOR) {
		result = avp_val.n^op_val.n;
	} else if (val->ops&AVPOPS_OP_BNOT) {
		result = ~op_val.n;
	} else {
		LOG(L_CRIT,"BUG:avpops:op_avp: unknown operation "
			"(flg=%d)\n",val->ops);
		goto error;
	}

	/* add the new avp */
	avp_val.n = result;
	if(add_avp(name_type3, avp_name3, avp_val)==-1 ) {
		LOG(L_ERR,"ERROR:avpops:op_avp: failed to create new avp\n");
		goto error;
	}

	/* cycle for the second value (only if avp can have multiple vals) */
	while((avp2!=NULL)
		&&(avp2=search_first_avp( name_type2, avp_name2, &op_val, avp2))!=0)
	{
		if(!(avp2->flags&AVP_VAL_STR))
			goto cycle2;
	}
	prev_avp = avp1;
	/* cycle for the first value -> next avp */
	while((avp1!=NULL)
		&&(avp1=search_first_avp(name_type1, avp_name1, &avp_val, avp1))!=0)
	{
		if (!(avp1->flags&AVP_VAL_STR))
		{
			if(val->ops&AVPOPS_FLAG_DELETE && prev_avp!=0)
			{
				destroy_avp(prev_avp);
				prev_avp = 0;
			}
			goto cycle1;
		}
	}
	DBG("DEBUG:avpops:op_avp: done\n");
	if(val->ops&AVPOPS_FLAG_DELETE && prev_avp!=0)
	{
		destroy_avp(prev_avp);
		prev_avp = 0;
	}
	return 1;

error:
	return -1;
}

int ops_is_avp_set(struct sip_msg* msg, struct fis_param *ap)
{
	struct usr_avp *avp;
	unsigned short    name_type;
	int_str avp_name;
	int_str avp_value;
	int index;
	
	/* get avp name */
	if(avpops_get_aname(msg, ap, &avp_name, &name_type)!=0)
	{
		LOG(L_ERR,
			"avpops:write_avp: error getting AVP name\n");
		return -1;
	}

	/* get avp index */
	if(xl_get_spec_index(&ap->sval, &index)!=0)
	{
		LOG(L_ERR,
			"avpops:write_avp: error getting AVP index\n");
		return -1;
	}
	
	avp=search_first_avp(name_type, avp_name, &avp_value, 0);
	if(avp==0)
		return -1;
	
	do {
		/* last index [-1] or all [*] go here as well */
		if(index<=0)
		{
			if(ap->ops&AVPOPS_FLAG_ALL)
				return 1;
			if((ap->ops&AVPOPS_FLAG_CASTS && !(avp->flags&AVP_VAL_STR))
					||(ap->ops&AVPOPS_FLAG_CASTN && avp->flags&AVP_VAL_STR))
				return -1;
			if(ap->ops&AVPOPS_FLAG_EMPTY)
			{
				if(avp->flags&AVP_VAL_STR)
				{
					if(avp_value.s.s==0 || avp_value.s.len==0)
						return 1;
					else
						return -1;
				} else {
					if(avp_value.n==0)
						return 1;
					else
						return -1;
				}
			}
			return 1;
		}
		index--;
	} while ((avp=search_first_avp(name_type, avp_name, &avp_value, avp))!=0);
	
	return -1;
}

