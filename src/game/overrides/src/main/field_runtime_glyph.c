/* Port replacement for the decompilation's field_runtime_glyph.c, which keeps
 * these two helpers as MIPS assembly. This is its behavioural C reference
 * made compilable. */
#include "main/field_runtime.h"
#include "common/gpu_packet.h"

#define FIELD_GLYPH_SIZE 8
#define FIELD_TEXT_OT_OFFSET 4
#define GPU_LINK_ADDRESS_MASK 0x00FFFFFFu
#define GPU_LINK_LENGTH_MASK 0xFF000000u

/** @brief Four-word sprite payload preceded by its GPU linked-list tag. */
typedef struct FieldGlyphPrimitive
{
    u32 tag;
    u32 color_code;
    u32 position;
    u32 texcoord_clut;
    u32 size;
} FieldGlyphPrimitive;

void field_draw_glyph(s32 character_arg, s32 ot_depth, s32 clut_offset)
{
    u8 character = (u8)character_arg;
    FieldGlyphPrimitive* primitive;
    u_long* ot_entry;
    u32 clut;
    u32 u;
    u32 v;

    if (character != ' ')
    {
        primitive = (FieldGlyphPrimitive*)g_field_primitive_cursor;
        clut = g_text_clut_base + clut_offset;
        u = 0x80 + (character & 0x0F) * FIELD_GLYPH_SIZE;
        v = 0xE0 + ((character - 0x20) & 0xF0) / 2;
        primitive->color_code = 0x66000000u | GPU_TINT_NEUTRAL;
        primitive->position = (g_text_cursor_y << 16) | g_text_cursor_x;
        /* Keep the full V result: the original packs it without a byte mask. */
        primitive->texcoord_clut = (clut << 16) | (v << 8) | u;
        primitive->size = (FIELD_GLYPH_SIZE << 16) | FIELD_GLYPH_SIZE;
        ot_entry = (u_long*)&g_field_current_render_half->ordering_table[ot_depth + FIELD_TEXT_OT_OFFSET];
        primitive->tag = 0x04000000u | (*ot_entry & GPU_LINK_ADDRESS_MASK);
        *ot_entry = (*ot_entry & GPU_LINK_LENGTH_MASK) | ((u32)primitive & GPU_LINK_ADDRESS_MASK);
        g_field_primitive_cursor = (void*)(primitive + 1);
    }
    g_text_cursor_x = (g_text_cursor_x + FIELD_GLYPH_SIZE);
}

void field_draw_hex_byte_masked(s32 value, s32 x, s32 y, s32 ot_depth, s32 clut_offset)
{
    g_text_cursor_x = x;
    g_text_cursor_y = y;
    field_draw_glyph(g_hex_digit_table[(value & 0xF0) >> 4], ot_depth, clut_offset);
    field_draw_glyph(g_hex_digit_table[value & 0x0F], ot_depth, clut_offset);
}
