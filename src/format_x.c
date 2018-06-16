#include "postgres.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "lib/stringinfo.h"

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

Datum format_x(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(format_x);

Datum format_x(PG_FUNCTION_ARGS) {
  char output[] = "Hello World";

  text *output_text;
  output_text = cstring_to_text_with_len(output, strlen(output));
  PG_RETURN_TEXT_P(output_text);
}
