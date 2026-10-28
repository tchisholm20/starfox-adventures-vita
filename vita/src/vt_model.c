/* vt_model.c - vita_tool_main.c configured for upstream's model_view.c. */
#define VT_TOOL          model_view_main
#define VT_NAME          "model_view"
#define VT_LOG           "ux0:data/stairfax_model.txt"
#define VT_SHOT          "ux0:data/stairfax_model.bmp"
#define VT_ARGS          "ux0:data/stairfax/args.txt"
/* No --index: upstream's own default, the largest UNSKINNED model. */
#define VT_DEFAULT_ARGS  "--dir", "animtest"
#define VT_CLEAR         26, 28, 33      /* rhi_clear(0.10, 0.11, 0.13) */
#include "vita_tool_main.c"
