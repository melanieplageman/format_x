#include "postgres.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "lib/stringinfo.h"
#include "hstore.h"
#include "access/htup_details.h" /* HeapTupleHeader, HeapTupleHeaderGet*(), heap_getattr() */
#include "catalog/pg_type.h" /* Oid constants */
#include "utils/jsonb.h"
#include "utils/lsyscache.h" /* getTypeOutputInfo(), type_is_rowtype() */
#include "utils/typcache.h" /* lookup_rowtype_tupdesc() */

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

Datum format_x(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(format_x);

typedef HStore *(*hstoreUpgradeF)(Datum);
typedef int (*hstoreFindKeyF)(HStore *, int *, char *, int);

typedef struct {
  int parameter; // a 0 indicates no parameter
  char *key; // NULL indicates no key
  int keylen; // a 0 keylen indicates no key
  bool flag;
  int width;
  int precision;
  char type;
} FormatInfo;

typedef struct {
  bool funcvariadic;

  int nargs;

  FunctionCallInfo fcinfo;

  Datum *elements;
  bool *nulls;
  Oid element_type;
} ArgumentData;

typedef struct {
  Datum item;
  Oid typid;
  bool isNull;
} Object;

/* Returns a formatted string when provided with named arguments */
void format_engine(FormatInfo *info, StringInfoData *output, ArgumentData *argdata);

/* Lookup an attribute by key (with length keylen) in object */
/* The result and result type is returned by rewriting members of object */
void format_lookup(Object *object, char *key, int keylen);
void record_lookup(Object *object, char *key, int keylen);
void jsonb_lookup(Object *object, char *key, int keylen);
void json_lookup(Object *object, char *key, int keylen);
void hstore_lookup(Object *object, char *key, int keylen);

/* Parse the optional portions of the format specifier */
char *option_format(StringInfoData *output, char *string, int length, int width, bool align_to_left);

/* Populate ArgumentData from FunctionCallInfo; copied from text_format() */
void make_argument_data(ArgumentData *argdata, FunctionCallInfo fcinfo);

/* Consume an ArgumentData and a position and return the datum at that position */
Datum getarg(ArgumentData *argdata, int parameter, Oid *typid, bool *isNull);

/* Copied from text_format_parse_digits() */
static inline void accumulate_number(int *old, char c);

/* Check if typid belongs to hstore as hstore's oid is not constant */
bool is_hstore(Oid typid);

/* findJsonbValueFromContainerLen() is static and must be copied here */
/* findJsonbValueFromContainerLen() is a findJsonbValueFromContainer() wrapper that sets up JsonbValue key string. */
static JsonbValue *findJsonbValueFromContainerLen(JsonbContainer *container,
                                                           uint32 flags,
                                                           char *key,
                                                           uint32 keylen);

/* For typid; GetAttributeByName() does not provide typid */
Datum GetAttributeAndTypeByName(HeapTupleHeader tuple, const char *attname, Oid *atttypid, bool *isNull);

typedef enum _format_state_t {
  FORMAT_NORMAL,
  FORMAT_PARAMETER_0,
  FORMAT_PARAMETER_1,
  FORMAT_PARAMETER_2,
  FORMAT_PARAMETER_3,
  FORMAT_FLAGS,
  FORMAT_WIDTH_0,
  FORMAT_WIDTH_1,
  FORMAT_PRECISION_0,
  FORMAT_PRECISION_1,
  FORMAT_DONE,
  FORMAT_UNEXPECTED,
} format_state_t;

format_state_t read_parameter_0(char* cp, FormatInfo *info);
format_state_t read_parameter_1(char *cp, FormatInfo *info);
format_state_t read_parameter_2(char *cp, FormatInfo *info);
format_state_t read_parameter_3(char *cp, FormatInfo *info);
format_state_t read_flags(char *cp, FormatInfo *info);
format_state_t read_width_0(char *cp, FormatInfo *info);
format_state_t read_width_1(char *cp, FormatInfo *info);
format_state_t read_precision_0(char *cp, FormatInfo *info);
format_state_t read_precision_1(char *cp, FormatInfo *info);

Datum format_x(PG_FUNCTION_ARGS) {
  text *format_string_text;
  char *cp, *startp, *endp;
  format_state_t s = FORMAT_NORMAL;
  format_state_t next;
  int last_parameter = 0;
  ArgumentData argdata = {
    .fcinfo = fcinfo };
  FormatInfo info;
  StringInfoData output;

  /* When format string is null, immediately return null */
  if (PG_ARGISNULL(0))
    PG_RETURN_NULL();

  make_argument_data(&argdata, fcinfo);

  format_string_text = PG_GETARG_TEXT_PP(0);
  startp = VARDATA_ANY(format_string_text);
  endp = startp + VARSIZE_ANY_EXHDR(format_string_text);

  initStringInfo(&output);

  for (cp = startp; cp < endp; cp++) {
    switch (s) {
      case FORMAT_NORMAL:
        if (*cp != '%') {
          appendStringInfoCharMacro(&output, *cp);
          continue;
        }

        next = FORMAT_PARAMETER_0;
        break;
      case FORMAT_PARAMETER_0:
        if (*cp == '%') {
          appendStringInfoCharMacro(&output, *cp);
          next = FORMAT_NORMAL;
        } else {
          info.parameter = 0;
          info.key = NULL;
          info.keylen = 0;
          info.flag = 0;
          info.width = 0;
          info.precision = 0;

          next = read_parameter_0(cp, &info);
        }
        break;
      case FORMAT_PARAMETER_1:
        next = read_parameter_1(cp, &info);
        break;
      case FORMAT_PARAMETER_2:
        next = read_parameter_2(cp, &info);
        break;
      case FORMAT_PARAMETER_3:
        next = read_parameter_3(cp, &info);
        break;
      case FORMAT_FLAGS:
        next = read_flags(cp, &info);
        break;
      case FORMAT_WIDTH_0:
        next = read_width_0(cp, &info);
        break;
      case FORMAT_WIDTH_1:
        next = read_width_1(cp, &info);
        break;
      case FORMAT_PRECISION_0:
        next = read_precision_0(cp, &info);
        break;
      case FORMAT_PRECISION_1:
        next = read_precision_1(cp, &info);
        break;
      default:
        next = FORMAT_UNEXPECTED;
        break;
    }

    if (next == FORMAT_UNEXPECTED)
      elog(ERROR, "ERROR");

    if (next == FORMAT_DONE) {
      if (info.parameter != 0) {
        last_parameter = info.parameter;
      } else {
        if (info.key == NULL || info.keylen == 0) {
          info.parameter = ++last_parameter;
        } else {
          if (last_parameter == 0)
            last_parameter++;
          info.parameter = last_parameter;
        }
      }
      format_engine(&info, &output, &argdata);
      next = FORMAT_NORMAL;
    }

    s = next;
  }

  if (s != FORMAT_NORMAL)
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("unterminated format_x() type specifier"),
                    errhint("For a single \"%%\" use \"%%%%\".")));

  text *output_text;
  output_text = cstring_to_text_with_len(output.data, output.len);
  pfree(output.data);
  PG_RETURN_TEXT_P(output_text);
}

void format_engine(FormatInfo *info, StringInfoData *output, ArgumentData *argdata) {
  Object object;
  bool typIsVarlena;
  FmgrInfo typoutputfinfo;
  Oid typoutputfunc;
  char *val;
  int vallen;

  object.isNull = false;
  object.item = getarg(argdata, info->parameter, &object.typid, &object.isNull);

  /* Handle lookup if there is a key */
  if (info->key != NULL || info->keylen != 0) {
    char *keycpy = palloc(info->keylen + 1);
    keycpy[info->keylen] = '\0';
    memcpy(keycpy, info->key, info->keylen);

    size_t numchars = 0;
    for (size_t i = 0; i <= info->keylen; i++) {
      if (keycpy[i] == '.' || i == info->keylen) {
        keycpy[i] = '\0';
        format_lookup(&object, keycpy, numchars);
        keycpy = keycpy + numchars + 1;
        numchars = 0;
      }
      else {
        numchars++;
      }
    }
  }

  if (object.isNull) {
    if (info->type == 'I') {
      ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("null values cannot be formatted as an SQL identifier")));
    }
    else if (info->type == 'L') {
      val = "NULL";
      info->type = 's';
    }
    else if (info->type == 's') {
      val = "";
    }
  }
  else {
    /* For floats, trim if precision (precision is only formatting done before conversion to string) */
    if (info->precision != 0 && (object.typid == FLOAT4OID || object.typid == FLOAT8OID)) {
      object.item = DirectFunctionCall2(numeric_round, object.item, info->precision);
    }

    /* Get the appropriate typOutput function */
    getTypeOutputInfo(object.typid, &typoutputfunc, &typIsVarlena);
    fmgr_info(typoutputfunc, &typoutputfinfo);

    val = OutputFunctionCall(&typoutputfinfo, object.item);
  }

  vallen = strlen(val);

  /* Once val and vallen have been retrieved and converted, move on to other format specifiers */

  /* Need to allocate memory for a new, null-terminated string */
  /* The return value from hstore_lookup() and jsonb getter are not necessarily null-terminated */
  char *string = palloc(vallen * sizeof(char) + 1);
  memcpy(string, val, vallen);
  string[vallen] = '\0';
  int length = vallen;

  if (info->type == 'I') {
    /* quote_identifier() sometimes returns a palloc'd string and sometimes returns the original string */
    string = (char *) quote_identifier(string);
    length = strlen(string);
  }
  else if (info->type == 'L') {
    string = (char *) quote_literal_cstr(string);
    length = strlen(string);
  }

  string = option_format(output, string, length, info->width, info->flag);
  if (info->type == 'L') {
    pfree(string);
  }
}

void format_lookup(Object *object, char *key, int keylen) {
  if (key == NULL || keylen == 0)
    return;

  if (object->isNull) {
    ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                    errmsg("null arguments cannot be looked up in, so cannot be passed for named parameters")));
  }

  if (type_is_rowtype(object->typid)) {
    record_lookup(object, key, keylen);
  }

  else if (object->typid == JSONBOID) {
    jsonb_lookup(object, key, keylen);
  }

  else if (is_hstore(object->typid)) {
    hstore_lookup(object, key, keylen);
  }

  else {
    elog(ERROR, "Invalid argument type for key \"%s\"", key);
  }
}

void record_lookup(Object *object, char *key, int keylen) {
  HeapTupleHeader record = DatumGetHeapTupleHeader(object->item);
  object->item = GetAttributeAndTypeByName(record, key, &object->typid, &object->isNull);
}

void jsonb_lookup(Object *object, char *key, int keylen) {
  Jsonb *jb = DatumGetJsonb(object->item);
  JsonbValue *v;

  v = findJsonbValueFromContainerLen(&jb->root, JB_FOBJECT, key, keylen);
  if (v == NULL) {
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("key \"%*s\" does not exist", keylen, key)));
  }

  switch (v->type) {
    case jbvNull:
      object->isNull = true;
      break;
    case jbvString:
      object->item = (Datum) cstring_to_text_with_len(v->val.string.val, v->val.string.len);
      object->typid = TEXTOID;
      break;
    case jbvNumeric:
      object->item = (Datum) v->val.numeric;
      object->typid = NUMERICOID;
      break;
    case jbvBool:
      object->item = (Datum) v->val.boolean;
      object->typid = BOOLOID;
      break;
    case jbvArray:
    case jbvObject:
    case jbvBinary:
      object->item = (Datum) JsonbValueToJsonb(v);
      break;
    default:
      elog(ERROR, "unrecognized jsonb type: %d", (int) v->type);
  }
}

bool is_hstore(Oid typid) {
  bool typIsVarlena;
  Oid typoutputfunc;
  FmgrInfo typoutputfinfo;
  PGFunction hstore_out;

  getTypeOutputInfo(typid, &typoutputfunc, &typIsVarlena);
  fmgr_info(typoutputfunc, &typoutputfinfo);

  hstore_out = load_external_function("hstore", "hstore_out", false, NULL);

  return typoutputfinfo.fn_addr == hstore_out;
}

void hstore_lookup(Object *object, char *key, int keylen) {
  hstoreUpgradeF hstoreUpgrade = (hstoreUpgradeF) load_external_function(
      "hstore", "hstoreUpgrade", true, NULL);
  HStore *hs = hstoreUpgrade(object->item);

  int (*hstoreFindKey) (HStore*, int*, char*, int) = (int (*)(HStore*, int*, char*, int)) load_external_function("hstore", "hstoreFindKey", true, NULL);
  int idx = hstoreFindKey(hs, NULL, key, keylen);
  
  /* If key is not found, generate error */
  if (idx < 0) {
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("key \"%*s\" does not exist", keylen, key)));
  }

  if (HSTORE_VALISNULL(ARRPTR(hs), idx)) {
    object->isNull = true;
  }

  object->item = (Datum) cstring_to_text_with_len(HSTORE_VAL(ARRPTR(hs), STRPTR(hs), idx), HSTORE_VALLEN(ARRPTR(hs), idx));
  object->typid = TEXTOID;
}

char *option_format(StringInfoData *output, char *string, int length, int width, bool align_to_left) {
  if (width == 0) {
    appendBinaryStringInfo(output, string, length);
    return string;
  }

  if (align_to_left) {
    // left justify
    appendBinaryStringInfo(output, string, length);
    if (length < width) {
      appendStringInfoSpaces(output, width - length);
    }
  }
  else {
    // right justify
    if (length < width) {
      appendStringInfoSpaces(output, width - length);
    }
    appendBinaryStringInfo(output, string, length);
  }
  return string;
}

Datum getarg(ArgumentData *argdata, int parameter, Oid *typid, bool *isNull) {
  FunctionCallInfo fcinfo = argdata->fcinfo;
  Datum arg;

  if (parameter >= argdata->nargs) {
    ereport(ERROR,
                  (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                   errmsg("too few arguments for format_x()")));
  }

  /* Get the value and type of the selected argument  */
  if (!argdata->funcvariadic) {
    arg = PG_GETARG_DATUM(parameter);
    *isNull = PG_ARGISNULL(parameter);
    *typid = get_fn_expr_argtype(argdata->fcinfo->flinfo, parameter);
  }
  else {
    arg = argdata->elements[parameter - 1];
    *isNull = argdata->nulls[parameter - 1];
    *typid = argdata->element_type;
  }

  if (!OidIsValid(*typid)) {
    elog(ERROR, "could not determine data type of format_x() input");
  }

  return arg;
}


format_state_t
read_parameter_0(char* cp, FormatInfo *info) {
  switch (*cp) {
    case '0':
      /* Explicit 0 for argument index is immediately refused */
      ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                      errmsg("format_x specifies argument 0, but arguments are numbered from 1")));
      return FORMAT_UNEXPECTED;

    case '1': case '2': case '3':
    case '4': case '5': case '6':
    case '7': case '8': case '9':
      info->parameter = *cp - '0';
      return FORMAT_PARAMETER_1;

    case '(':
      info->key = cp + 1;
      return FORMAT_PARAMETER_2;

    case '-':
      info->flag = true;
      return FORMAT_FLAGS;

    case '.':
      info->precision = 0;
      return FORMAT_PRECISION_1;

    case 's':
    case 'I':
    case 'L':
      info->type = *cp;
      return FORMAT_DONE;
  }

  ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                  errmsg("unrecognized format_x() type specifier \"%c\"",
                         *cp),
                  errhint("For a single \"%%\" use \"%%%%\".")));
  return FORMAT_UNEXPECTED;
}

format_state_t
read_parameter_1(char *cp, FormatInfo *info) {
  switch (*cp) {
    case '0': case '1': case '2': case '3':
    case '4': case '5': case '6':
    case '7': case '8': case '9':
      accumulate_number(&info->parameter, *cp);
      return FORMAT_PARAMETER_1;

    case '(':
      info->key = cp + 1;
      return FORMAT_PARAMETER_2;

    case '$':
      return FORMAT_PARAMETER_3;

    case '.':
      info->width = info->parameter;
      info->parameter = 0;
      info->precision = 0;
      return FORMAT_PRECISION_1;

    case 's':
    case 'I':
    case 'L':
      info->width = info->parameter;
      info->parameter = 0;
      info->type = *cp;
      return FORMAT_DONE;
  }

  ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                  errmsg("unrecognized format_x() type specifier \"%c\"",
                         *cp),
                  errhint("For a single \"%%\" use \"%%%%\".")));

  return FORMAT_UNEXPECTED;
}

format_state_t
read_parameter_2(char *cp, FormatInfo *info) {
  switch (*cp) {
    case ')':
      return FORMAT_PARAMETER_3;

    case '(':
      ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                      errmsg("key cannot contain '('")));
      return FORMAT_UNEXPECTED;

    default:
      info->keylen++;
  }

  return FORMAT_PARAMETER_2;
}

format_state_t
read_parameter_3(char *cp, FormatInfo *info) {
  switch (*cp) {
    case '-':
      info->flag = true;
      return FORMAT_FLAGS;

    case '1': case '2': case '3':
    case '4': case '5': case '6':
    case '7': case '8': case '9':
      info->width = *cp - '0';
      return FORMAT_WIDTH_1;

    case '.':
      info->precision = 0;
      return FORMAT_PRECISION_1;

    case 's':
    case 'I':
    case 'L':
      info->type = *cp;
      return FORMAT_DONE;
  }

  ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                  errmsg("unrecognized format_x() type specifier \"%c\"",
                         *cp),
                  errhint("For a single \"%%\" use \"%%%%\".")));

  return FORMAT_PARAMETER_2;
}

format_state_t
read_flags(char *cp, FormatInfo *info) {
  switch (*cp) {
    case '-':
      info->flag = true;
      return FORMAT_FLAGS;

    case '1': case '2': case '3':
    case '4': case '5': case '6':
    case '7': case '8': case '9':
      info->width = *cp - '0';
      return FORMAT_WIDTH_1;

    case '.':
      info->precision = 0;
      return FORMAT_PRECISION_1;

    case 's':
    case 'I':
    case 'L':
      info->type = *cp;
      return FORMAT_DONE;
  }

  ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                  errmsg("unrecognized format_x() type specifier \"%c\"",
                         *cp),
                  errhint("For a single \"%%\" use \"%%%%\".")));

  return FORMAT_UNEXPECTED;
}

format_state_t
read_width_0(char *cp, FormatInfo *info) {
  switch (*cp) {
    case '1': case '2': case '3':
    case '4': case '5': case '6':
    case '7': case '8': case '9':
      info->width = *cp - '0';
      return FORMAT_WIDTH_1;

    case '.':
      info->precision = 0;
      return FORMAT_PRECISION_1;

    case 's':
    case 'I':
    case 'L':
      info->type = *cp;
      return FORMAT_DONE;
  }

  ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                  errmsg("unrecognized format_x() type specifier \"%c\"",
                         *cp),
                  errhint("For a single \"%%\" use \"%%%%\".")));

  return FORMAT_UNEXPECTED;
}

format_state_t
read_width_1(char *cp, FormatInfo *info) {
  switch (*cp) {
    case '0': case '1': case '2': case '3':
    case '4': case '5': case '6':
    case '7': case '8': case '9':
      accumulate_number(&info->width, *cp);
      return FORMAT_WIDTH_1;

    case '.':
      info->precision = 0;
      return FORMAT_PRECISION_1;

    case 's':
    case 'I':
    case 'L':
      info->type = *cp;
      return FORMAT_DONE;
  }

  ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                  errmsg("unrecognized format_x() type specifier \"%c\"",
                         *cp),
                  errhint("For a single \"%%\" use \"%%%%\".")));

  return FORMAT_UNEXPECTED;
}

format_state_t
read_precision_0(char *cp, FormatInfo *info) {
  switch (*cp) {
    case '.':
      info->precision = 0;
      return FORMAT_PRECISION_1;

    case 's':
    case 'I':
    case 'L':
      info->type = *cp;
      return FORMAT_DONE;
  }

  ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                  errmsg("unrecognized format_x() type specifier \"%c\"",
                         *cp),
                  errhint("For a single \"%%\" use \"%%%%\".")));

  return FORMAT_UNEXPECTED;
}

format_state_t
read_precision_1(char *cp, FormatInfo *info) {
  switch (*cp) {
    case '0': case '1': case '2': case '3':
    case '4': case '5': case '6':
    case '7': case '8': case '9':
      accumulate_number(&info->precision, *cp);
      return FORMAT_WIDTH_1;

    case 's':
    case 'I':
    case 'L':
      info->type = *cp;
      return FORMAT_DONE;
  }

  ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                  errmsg("unrecognized format_x() type specifier \"%c\"",
                         *cp),
                  errhint("For a single \"%%\" use \"%%%%\".")));

  return FORMAT_UNEXPECTED;
}

static inline void accumulate_number(int *old, char c) {
  int new = *old * 10 + (c - '0');
  if (new / 10 != *old) /* overflow? */
    ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                    errmsg("number is out of range")));
  *old = new;
}

static JsonbValue *
findJsonbValueFromContainerLen(JsonbContainer *container, uint32 flags,
                                                           char *key, uint32 keylen)
{
        JsonbValue      k;

        k.type = jbvString;
        k.val.string.val = key;
        k.val.string.len = keylen;

        return findJsonbValueFromContainer(container, flags, &k);
}

Datum
GetAttributeAndTypeByName(HeapTupleHeader tuple, const char *attname, Oid *atttypid, bool *isNull)
{
        AttrNumber      attrno;
        Datum           result;
        Oid                     tupType;
        int32           tupTypmod;
        TupleDesc       tupDesc;
        HeapTupleData tmptup;
        int                     i;

        if (attname == NULL)
                elog(ERROR, "invalid attribute name");

        if (isNull == NULL)
                elog(ERROR, "a NULL isNull pointer was passed");

        if (tuple == NULL)
        {
                /* Kinda bogus but compatible with old behavior... */
                *isNull = true;
                return (Datum) 0;
        }

        tupType = HeapTupleHeaderGetTypeId(tuple);
        tupTypmod = HeapTupleHeaderGetTypMod(tuple);
        tupDesc = lookup_rowtype_tupdesc(tupType, tupTypmod);

        attrno = InvalidAttrNumber;
        for (i = 0; i < tupDesc->natts; i++)
        {
                if (namestrcmp(&(tupDesc->attrs[i]->attname), attname) == 0)
                {
                        attrno = tupDesc->attrs[i]->attnum;
                        *atttypid = tupDesc->attrs[i]->atttypid;
                        break;
                }
        }

        if (attrno == InvalidAttrNumber)
                elog(ERROR, "attribute \"%s\" does not exist", attname);

        /*
         * heap_getattr needs a HeapTuple not a bare HeapTupleHeader.  We set all
         * the fields in the struct just in case user tries to inspect system
         * columns.
         */
        tmptup.t_len = HeapTupleHeaderGetDatumLength(tuple);
        ItemPointerSetInvalid(&(tmptup.t_self));
        tmptup.t_tableOid = InvalidOid;
        tmptup.t_data = tuple;

        result = heap_getattr(&tmptup,
                                                  attrno,
                                                  tupDesc,
                                                  isNull);

        ReleaseTupleDesc(tupDesc);

        return result;
}

void make_argument_data(ArgumentData *argdata, FunctionCallInfo fcinfo) {
	bool		funcvariadic;
	int			nargs;
	Datum	   *elements = NULL;
	bool	   *nulls = NULL;
	Oid			element_type = InvalidOid;

	/* If argument is marked VARIADIC, expand array into elements */
	if (get_fn_expr_variadic(fcinfo->flinfo))
	{
		ArrayType  *arr;
		int16		elmlen;
		bool		elmbyval;
		char		elmalign;
		int			nitems;

		/* Should have just the one argument */
		Assert(PG_NARGS() == 2);

		/* If argument is NULL, we treat it as zero-length array */
		if (PG_ARGISNULL(1))
			nitems = 0;
		else
		{
			/*
			 * Non-null argument had better be an array.  We assume that any
			 * call context that could let get_fn_expr_variadic return true
			 * will have checked that a VARIADIC-labeled parameter actually is
			 * an array.  So it should be okay to just Assert that it's an
			 * array rather than doing a full-fledged error check.
			 */
			Assert(OidIsValid(get_base_element_type(get_fn_expr_argtype(fcinfo->flinfo, 1))));

			/* OK, safe to fetch the array value */
			arr = PG_GETARG_ARRAYTYPE_P(1);

			/* Get info about array element type */
			element_type = ARR_ELEMTYPE(arr);
			get_typlenbyvalalign(element_type,
								 &elmlen, &elmbyval, &elmalign);

			/* Extract all array elements */
			deconstruct_array(arr, element_type, elmlen, elmbyval, elmalign,
							  &elements, &nulls, &nitems);
		}

		nargs = nitems + 1;
		funcvariadic = true;
	}
	else
	{
		/* Non-variadic case, we'll process the arguments individually */
		nargs = PG_NARGS();
		funcvariadic = false;
	}

        argdata->funcvariadic = funcvariadic;
        argdata->nargs = nargs;
        argdata->elements = elements;
        argdata->nulls = nulls;
        argdata->element_type = element_type;
}
