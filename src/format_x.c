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

/* This struct holds data about each format specifier in the format string */ 
/* After processing and appending the result to the output buffer, it is overwritten by the next format specifier's data
 * Note that the format string data is stored elsewhere */
typedef struct {
  int parameter; // a 0 indicates no parameter
  char *key; // NULL indicates no key
  int keylen; // a 0 keylen indicates no key
  bool flag;
  int width;
  int precision;
  char type;
} FormatSpecifierData;

/* This struct holds both info about the format args */
/* as well as the arg data in the case of a variadic argument */
typedef struct {
  // Variadic info
  bool funcvariadic;

  int nargs;

  FunctionCallInfo fcinfo;

  Datum *elements;
  bool *nulls;
  Oid element_type;

  // HStore info
  void *filehandle;
  hstoreFindKeyF hstoreFindKey;
  hstoreUpgradeF hstoreUpgrade;
  Oid hstoreOid;
} FormatargInfoData;

typedef struct {
  Datum item;
  Oid typid;
  bool isNull;
} Object;

/* Read contiguous digits as a decimal number */
static bool format_read_digits(char **cpp, char *endp, int *number);

/* Read a format specifier (generally following the SUS printf specification) */
static char *format_read_specifier(char *cp, char *endp, FormatSpecifierData *spec);

/* Returns a formatted string when provided with named arguments */
void format_engine(FormatSpecifierData *specifierdata, StringInfoData *output, FormatargInfoData *arginfodata);

/* Lookup an attribute by key (with length keylen) in object */
/* The result and result type is returned by rewriting members of object */
void format_lookup(Object *object, FormatargInfoData *arginfodata, char *key, int keylen);
void record_lookup(Object *object, char *key, int keylen);
void jsonb_lookup(Object *object, char *key, int keylen);
void json_lookup(Object *object, char *key, int keylen);
void hstore_lookup(Object *object, FormatargInfoData *arginfodata, char *key, int keylen);

/* Parse the optional portions of the format specifier */
char *option_format(StringInfoData *output, char *string, int length, int width, bool align_to_left);

/* Populate FormatargInfoData from FunctionCallInfo; copied from text_format() */
void make_argument_data(FormatargInfoData *arginfodata, FunctionCallInfo fcinfo);

/* Consume an FormatargInfoData and a position and return the datum at that position */
Datum getarg(FormatargInfoData *arginfodata, int parameter, Oid *typid, bool *isNull);

/* Check if typid belongs to hstore as hstore's oid is not constant */
bool is_hstore(Oid typid, FormatargInfoData *arginfodata);

/* findJsonbValueFromContainerLen() is static and must be copied here */
/* findJsonbValueFromContainerLen() is a findJsonbValueFromContainer() wrapper that sets up JsonbValue key string. */
static JsonbValue *findJsonbValueFromContainerLen(JsonbContainer *container,
                                                           uint32 flags,
                                                           char *key,
                                                           uint32 keylen);

/* For typid; GetAttributeByName() does not provide typid */
Datum GetAttributeAndTypeByName(HeapTupleHeader tuple, const char *attname, Oid *atttypid, bool *isNull);

#define ADVANCE_READ_POINTER(cp, endp) do { \
  if (++(cp) >= (endp)) \
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), \
		    errmsg("unterminated format_x() type specifier"), \
		    errhint("For a single \"%%\" use \"%%%%\"."))); \
} while (0)

Datum format_x(PG_FUNCTION_ARGS) {
  text *format_string_text;
  char *cp, *startp, *endp;
  int last_parameter = 0;
  FormatargInfoData arginfodata = {
    .fcinfo = fcinfo };
  FormatSpecifierData spec;
  StringInfoData output;

  /* When format string is null, immediately return null */
  if (PG_ARGISNULL(0))
    PG_RETURN_NULL();

  make_argument_data(&arginfodata, fcinfo);

  arginfodata.filehandle = NULL;
  arginfodata.hstoreFindKey = NULL;
  arginfodata.hstoreUpgrade = NULL;
  arginfodata.hstoreOid = InvalidOid;

  format_string_text = PG_GETARG_TEXT_PP(0);
  startp = VARDATA_ANY(format_string_text);
  endp = startp + VARSIZE_ANY_EXHDR(format_string_text);

  initStringInfo(&output);

  /* Scan format string looking for format specifiers */
  for (cp = startp; cp < endp; cp++) {
    /* If it's not the start of a format specifier just copy it to output */
    if (*cp != '%') {
      appendStringInfoCharMacro(&output, *cp);
      continue;
    }
    ADVANCE_READ_POINTER(cp, endp);

    /* Easy case: %% outputs a single % */
    if (*cp == '%') {
      appendStringInfoCharMacro(&output, '%');
      continue;
    }

    cp = format_read_specifier(cp, endp, &spec);

    if (spec.parameter == 0) {
      if (spec.key == NULL || spec.keylen == 0)
        spec.parameter = ++last_parameter;
      else {
        if (last_parameter == 0)
          last_parameter++;
        spec.parameter = last_parameter;
      }
    } else last_parameter = spec.parameter;

    format_engine(&spec, &output, &arginfodata);
  }

  text *output_text;
  output_text = cstring_to_text_with_len(output.data, output.len);
  pfree(output.data);
  PG_RETURN_TEXT_P(output_text);
}

/*
 * Read contiguous digits as a decimal number.
 *
 * Returns true if some digits could be read.
 * The number is returned into *number, and *cpp is advanced to the next
 * character to be read.
 *
 * Note parsing invariant: at least one character is known to be available
 * before string end (endp) at entry, and this is still true at exit.
 */
static bool format_read_digits(char **cpp, char *endp, int *number) {
  bool found = false;
  int old = 0;

  while (**cpp >= '0' && **cpp <= '9') {
    int new = old * 10 + (**cpp - '0');

    if (new / 10 != old) /* overflow? */
      ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                      errmsg("number is out of range")));
    old = new;

    ADVANCE_READ_POINTER(*cpp, endp);
    found = true;
  }

  *number = old;
  return found;
}

/*
 * Read a format specifier (generally following the SUS printf specification).
 *
 * We have already advanced over the initial '%', and we are looking for
 * [parameter][(key)][flags][width]type.
 *
 * Inputs are cp (the position after '%') and endp (string end + 1).
 *
 * The format specifier details are returned in *spec.
 *
 * The function result is the last character position to be read, ie, the
 * location where the type character is.
 *
 * Note parsing invariant: at least one character is known to be available
 * before string end (endp) at entry, and this is still true at exit.
 */
static char *
format_read_specifier(char *cp, char *endp, FormatSpecifierData *spec) {
  int number;

  /* Set defaults for output specifier data */
  *spec = (FormatSpecifierData) {
    .parameter = 0,
    .key = NULL,
    .keylen = 0,
    .flag = 0,
    .width = 0,
    .precision = 0,
  };

  if (format_read_digits(&cp, endp, &number)) {
    if (*cp != '$' && *cp != '(') {
      /* The number isn't argument position so assume it's width and skip
       * everything before precision */
      spec->width = number;
      goto format_read_precision;
    }

    /* The number was argument position */
    spec->parameter = number;

    /* Explicit 0 for argument index is immediately refused */
    if (number == 0)
      ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                      errmsg("format specifies argument 0, but arguments are numbered from 1")));
  }

  /* There are only two possibilities at this point: either we just read a
   * number or we didn't. Since we use spec->parameter = 0 to indicate an
   * implicit parameter reference then anything else indicates that we just
   * read a number.
   *
   * The '$' is only allowed here if we just read a number and its presence
   * excludes the presence of a key. */

  if (*cp == '$' && spec->parameter > 0) {
    ADVANCE_READ_POINTER(cp, endp);
  } else if (*cp == '(') {
    /* The character after this must be the start of the key */
    ADVANCE_READ_POINTER(cp, endp);
    spec->key = cp;

    while (*cp != ')') {
      if (*cp == '(')
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("key cannot contain '('")));
      ADVANCE_READ_POINTER(cp, endp);
    }

    /* At this point cp points immediately after the key end */
    spec->keylen = cp - spec->key;
    ADVANCE_READ_POINTER(cp, endp);
  }

  /* Handle flags (only minus is supported now) */
  while (*cp == '-') {
    /* spec->flags |= FORMAT_FLAG_MINUS; */
    spec->flag = 1;
    ADVANCE_READ_POINTER(cp, endp);
  }

  /* Check for direct width specification */
  if (format_read_digits(&cp, endp, &number))
    spec->width = number;

format_read_precision:
  if (*cp == '.') {
    ADVANCE_READ_POINTER(cp, endp);

    if (!format_read_digits(&cp, endp, &spec->precision))
      ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                      errmsg(". must be followed by precision")));
  }

  /* Handle type (either 's', 'I', or 'L') */
  if (strchr("sIL", *cp) == NULL)
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("unrecognized format_x() type specifier \"%c\"",
                            *cp),
                    errhint("For a single \"%%\" use \"%%%%\".")));
  spec->type = *cp;

  return cp;
}

void format_engine(FormatSpecifierData *specifierdata, StringInfoData *output, FormatargInfoData *arginfodata) {
  Object object;
  Oid prev_typid = InvalidOid;
  FmgrInfo typoutputfinfo;
  char *val;
  int vallen;

  object.isNull = false;
  object.item = getarg(arginfodata, specifierdata->parameter, &object.typid, &object.isNull);

  /* Handle lookup if there is a key */
  if (specifierdata->key != NULL || specifierdata->keylen != 0) {
    char *keycpy = palloc(specifierdata->keylen + 1);
    keycpy[specifierdata->keylen] = '\0';
    memcpy(keycpy, specifierdata->key, specifierdata->keylen);

    size_t numchars = 0;
    for (size_t i = 0; i <= specifierdata->keylen; i++) {
      if (keycpy[i] == '.' || i == specifierdata->keylen) {
        keycpy[i] = '\0';
        format_lookup(&object, arginfodata, keycpy, numchars);
        keycpy = keycpy + numchars + 1;
        numchars = 0;
      }
      else {
        numchars++;
      }
    }
  }

  if (object.isNull) {
    if (specifierdata->type == 'I') {
      ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("null values cannot be formatted as an SQL identifier")));
    }
    else if (specifierdata->type == 'L') {
      val = "NULL";
      specifierdata->type = 's';
    }
    else if (specifierdata->type == 's') {
      val = "";
    }
  }
  else {
    /* For floats, trim if precision (precision is only formatting done before conversion to string) */
    if (specifierdata->precision != 0 && (object.typid == FLOAT4OID || object.typid == FLOAT8OID)) {
      object.item = DirectFunctionCall2(numeric_round, object.item, specifierdata->precision);
    }

    /* Get the appropriate typOutput function */
    if (object.typid != prev_typid) {
      bool typIsVarlena;
      Oid typoutputfunc;

      getTypeOutputInfo(object.typid, &typoutputfunc, &typIsVarlena);
      fmgr_info(typoutputfunc, &typoutputfinfo);
      prev_typid = object.typid;
    }

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

  if (specifierdata->type == 'I') {
    /* quote_identifier() sometimes returns a palloc'd string and sometimes returns the original string */
    string = (char *) quote_identifier(string);
    length = strlen(string);
  }
  else if (specifierdata->type == 'L') {
    string = (char *) quote_literal_cstr(string);
    length = strlen(string);
  }

  string = option_format(output, string, length, specifierdata->width, specifierdata->flag);
  if (specifierdata->type == 'L') {
    pfree(string);
  }
}

void format_lookup(Object *object, FormatargInfoData *arginfodata, char *key, int keylen) {
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

  else if (is_hstore(object->typid, arginfodata)) {
    hstore_lookup(object, arginfodata, key, keylen);
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

bool is_hstore(Oid typid, FormatargInfoData *arginfodata) {
  /* If HStore has been previously detected, skip lookup for oid */
  if (arginfodata->hstoreOid != InvalidOid) {
    return arginfodata->hstoreOid == typid;
  }

  bool typIsVarlena;
  Oid typoutputfunc;
  FmgrInfo typoutputfinfo;
  PGFunction hstore_out;

  getTypeOutputInfo(typid, &typoutputfunc, &typIsVarlena);
  fmgr_info(typoutputfunc, &typoutputfinfo);

  hstore_out = load_external_function("hstore", "hstore_out", false, &arginfodata->filehandle);

  if (typoutputfinfo.fn_addr == hstore_out) {
    arginfodata->hstoreOid = typid;
    return true;
  }
  
  return false;
}

void hstore_lookup(Object *object, FormatargInfoData *arginfodata, char *key, int keylen) {
  if (arginfodata->hstoreUpgrade == NULL) {
    arginfodata->hstoreUpgrade = (hstoreUpgradeF) lookup_external_function(arginfodata->filehandle, "hstoreUpgrade");
  }
  HStore *hs = arginfodata->hstoreUpgrade(object->item);

  if (arginfodata->hstoreFindKey == NULL) {
    int (*hstoreFindKey) (HStore*, int*, char*, int) = (int (*)(HStore*, int*, char*, int)) lookup_external_function(arginfodata->filehandle, "hstoreFindKey");
    arginfodata->hstoreFindKey = hstoreFindKey;
  }
  int idx = arginfodata->hstoreFindKey(hs, NULL, key, keylen);

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

Datum getarg(FormatargInfoData *arginfodata, int parameter, Oid *typid, bool *isNull) {
  FunctionCallInfo fcinfo = arginfodata->fcinfo;
  Datum arg;

  if (parameter >= arginfodata->nargs) {
    ereport(ERROR,
                  (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                   errmsg("too few arguments for format_x()")));
  }

  /* Get the value and type of the selected argument  */
  if (!arginfodata->funcvariadic) {
    arg = PG_GETARG_DATUM(parameter);
    *isNull = PG_ARGISNULL(parameter);
    *typid = get_fn_expr_argtype(arginfodata->fcinfo->flinfo, parameter);
  }
  else {
    arg = arginfodata->elements[parameter - 1];
    *isNull = arginfodata->nulls[parameter - 1];
    *typid = arginfodata->element_type;
  }

  if (!OidIsValid(*typid)) {
    elog(ERROR, "could not determine data type of format_x() input");
  }

  return arg;
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

void make_argument_data(FormatargInfoData *arginfodata, FunctionCallInfo fcinfo) {
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

        arginfodata->funcvariadic = funcvariadic;
        arginfodata->nargs = nargs;
        arginfodata->elements = elements;
        arginfodata->nulls = nulls;
        arginfodata->element_type = element_type;
}
