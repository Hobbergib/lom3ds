/* Port replacement: the original moves the stack into the scratchpad for
 * speed. A native stack is already fast, so both macros do nothing. */
#ifndef SDK_SPADSTK_H
#define SDK_SPADSTK_H
#define SetSpadStack(save_slot) ((void)(save_slot))
#define ResetSpadStack() ((void)0)
#endif
