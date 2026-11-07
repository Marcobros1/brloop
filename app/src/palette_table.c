#include "palette_tables.h"

/* Catalog from paletas-catalogo.md, colors as strings. PALETTE writes both
 * variants of one palette; SINGLE writes the only variant of a palette. */
#define PALETTE(name, lbg, lsurface, laccent, ltext, ltext_acc, lplay, ltext_play, lpause, ltext_pause, lfin, ltext_fin, \
                dbg, dsurface, daccent, dtext, dtext_acc, dplay, dtext_play, dpause, dtext_pause, dfin, dtext_fin) \
    { name, "Claro", lbg, lsurface, laccent, ltext, ltext_acc, lplay, ltext_play, lpause, ltext_pause, lfin, ltext_fin }, \
    { name, "Oscuro", dbg, dsurface, daccent, dtext, dtext_acc, dplay, dtext_play, dpause, dtext_pause, dfin, dtext_fin }
#define SINGLE(name, variant, bg, surface, accent, text, text_acc, play, text_play, pause, text_pause, fin, text_fin) \
    { name, variant, bg, surface, accent, text, text_acc, play, text_play, pause, text_pause, fin, text_fin }

static const palette_entry_t palette_table[] = {
    SINGLE("Super Mario Galaxy 2", "Claro",
           "#5FC2FB", "#77CBFC", "#05478D", "#000000", "#FFFFFF",
           "#1F5422", /* was #2E7D32 */ "#FFFFFF", "#594800", /* was #F5C400 */ "#FFFFFF", /* was #111111 */ "#1D4984", /* was #1F4E8C */ "#FFFFFF"),
    SINGLE("Super Mario Galaxy", "Oscuro",
           "#051747", "#04143C", "#9CC4FF", "#FFFFFF", "#000000",
           "#8FE39A", "#111111", "#F5C36B", "#111111", "#9DC3FF", "#111111"),
    PALETTE("Metroid Prime",
            "#D8C7A8", "#DECFB5", "#0A5D6B", "#000000", "#FFFFFF",
            "#1B612F", /* was #1E6B34 */ "#FFFFFF", "#824500", /* was #8A4A00 */ "#FFFFFF", "#1F4E8C", "#FFFFFF",
            "#473118", "#3C2A14", "#5FD3E6", "#FFFFFF", "#000000",
            "#8FE39A", "#111111", "#FFB36B", "#111111", "#9DC3FF", "#111111"),
    PALETTE("Metroid Prime 2",
            "#EEF0F2", "#F1F2F4", "#7F5DAA", "#000000", "#FFFFFF",
            "#207E2C", /* was #8FE39A */ "#FFFFFF", /* was #111111 */ "#95630A", /* was #F5C36B */ "#FFFFFF", /* was #111111 */ "#0061F9", /* was #9DC3FF */ "#FFFFFF" /* was #111111 */,
            "#030203", "#030203", "#A77BE0", "#FFFFFF", "#000000",
            "#8FE39A", "#111111", "#F5C36B", "#111111", "#9DC3FF", "#111111"),
    PALETTE("Metroid Prime 3",
            "#9FC8EA", "#ADD0ED", "#3E5067", "#000000", "#FFFFFF",
            "#175D21", /* was #8FE39A */ "#FFFFFF", /* was #111111 */ "#6F4907", /* was #F5C36B */ "#FFFFFF", /* was #111111 */ "#0048BB", /* was #9DC3FF */ "#FFFFFF" /* was #111111 */,
            "#591316", "#4C1013", "#7A98BE", "#FFFFFF", "#000000",
            "#8FE39A", "#111111", "#F5C36B", "#111111", "#9DC3FF", "#111111"),
    PALETTE("Zelda Wind Waker",
            "#A98E63", "#B69F7A", "#0C3028", "#000000", "#FFFFFF",
            "#0E3219", /* was #1E6B34 */ "#FFFFFF", "#432400", /* was #8A4A00 */ "#FFFFFF", "#112C4E", /* was #1F4E8C */ "#FFFFFF",
            "#90754C", "#A18A67", "#080D0B", "#000000", "#FFFFFF",
            "#040E06", /* was #8FE3A3 */ "#FFFFFF", /* was #111111 */ "#110B01", /* was #F5C36B */ "#FFFFFF", /* was #111111 */ "#000D1C", /* was #8EC5FF */ "#FFFFFF" /* was #111111 */),
    SINGLE("Zelda Skyward Sword", "Claro",
           "#AB8E2F", "#B89F4E", "#0E2C43", "#000000", "#FFFFFF",
           "#0E3118", /* was #1E6B34 */ "#FFFFFF", "#422300", /* was #8A4A00 */ "#FFFFFF", "#112B4C", /* was #1F4E8C */ "#FFFFFF"),
    PALETTE("Zelda Twilight Princess",
            "#A39A84", "#B1A996", "#2F3527", "#000000", "#FFFFFF",
            "#0F3B15", /* was #8FE39A */ "#FFFFFF", /* was #111111 */ "#462F05", /* was #F5C36B */ "#FFFFFF", /* was #111111 */ "#002E78", /* was #9DC3FF */ "#FFFFFF" /* was #111111 */,
            "#828081", "#959394", "#161812", "#000000", "#FFFFFF",
            "#071D0A", /* was #8FE39A */ "#FFFFFF", /* was #111111 */ "#221602", /* was #F5C36B */ "#FFFFFF", /* was #111111 */ "#00173C", /* was #9DC3FF */ "#FFFFFF" /* was #111111 */),
    PALETTE("Super Smash Bros. Melee",
            "#5E92C7", "#76A2CF", "#4F1114", "#000000", "#FFFFFF",
            "#0D2E17", /* was #1E6B34 */ "#FFFFFF", "#3E2100", /* was #8A4A00 */ "#FFFFFF", "#102849", /* was #1F4E8C */ "#FFFFFF",
            "#385877", "#304B65", "#FFBABC", "#FFFFFF", "#000000",
            "#8FE39A", "#111111", "#F5C36B", "#111111", "#ABCCFF", /* was #9DC3FF */ "#111111"),
    PALETTE("Smash Brawl",
            "#2D5BB8", "#264D9C", "#F5D4D9", "#FFFFFF", "#000000",
            "#AAE7BB", /* was #1E6B34 */ "#000000", /* was #FFFFFF */ "#FFD19C", /* was #8A4A00 */ "#000000", /* was #FFFFFF */ "#C8DBF3", /* was #1F4E8C */ "#000000" /* was #FFFFFF */,
            "#0F2A6B", "#0D245B", "#FF6670", "#FFFFFF", "#000000",
            "#8FE39A", "#111111", "#F5C36B", "#111111", "#9DC3FF", "#111111"),
    SINGLE("Mario Kart Wii", "Claro",
           "#F2F2F2", "#F4F4F4", "#1F62B0", "#000000", "#FFFFFF",
           "#2E7D32", "#FFFFFF", "#B3262E", "#FFFFFF", "#6B4A1F", "#FFFFFF"),
    SINGLE("Mario Kart Double Dash", "Claro",
           "#E9D8B4", "#ECDEBF", "#B82328", "#000000", "#FFFFFF",
           "#286D2C", /* was #2E7D32 */ "#FFFFFF", "#964B00", /* was #B85C00 */ "#FFFFFF", "#1F5FA8", "#FFFFFF"),
    PALETTE("Super Mario Sunshine",
            "#F2C12E", "#F4CA4D", "#185783", "#000000", "#FFFFFF",
            "#1B602F", /* was #1E6B34 */ "#FFFFFF", "#804400", /* was #8A4A00 */ "#FFFFFF", "#1F4E8C", "#FFFFFF",
            "#6A5514", "#5A4811", "#9BD4FF", "#FFFFFF", "#000000",
            "#8FE39A", "#111111", "#F5C671", /* was #F5C36B */ "#111111", "#9AD4FF", /* was #7FC8FF */ "#111111"),
    SINGLE("Animal Crossing", "Claro",
           "#A88B62", "#B59C7A", "#112E16", "#000000", "#FFFFFF",
           "#0D2F17", /* was #1E6B34 */ "#FFFFFF", "#402200", /* was #8A4A00 */ "#FFFFFF", "#10294A", /* was #1F4E8C */ "#FFFFFF"),
    PALETTE("Resident Evil 4",
            "#B8B4B0", "#C3BFBC", "#7C2B3A", "#000000", "#FFFFFF",
            "#175128", /* was #1E6B34 */ "#FFFFFF", "#6D3A00", /* was #8A4A00 */ "#FFFFFF", "#1C4780", /* was #1F4E8C */ "#FFFFFF",
            "#5A1A22", "#4C161D", "#D8808F", "#FFFFFF", "#000000",
            "#8FE39A", "#111111", "#F5C36B", "#111111", "#9DC3FF", "#111111"),
    PALETTE("Wii Sports Resort",
            "#3FA3DE", "#5CB1E3", "#01335B", "#000000", "#FFFFFF",
            "#113B1D", /* was #1E6B34 */ "#FFFFFF", "#4F2A00", /* was #8A4A00 */ "#FFFFFF", "#14335C", /* was #1F4E8C */ "#FFFFFF",
            "#235B7C", "#1E4D69", "#B4CEF4", "#FFFFFF", "#000000",
            "#8FE3A3", "#111111", "#F5C36B", "#111111", "#A1CFFF", /* was #8EC5FF */ "#111111"),
    PALETTE("Wii Fit",
            "#F4F4F2", "#F6F6F4", "#556E00", "#000000", "#FFFFFF",
            "#1E6B34", "#FFFFFF", "#8A4A00", "#FFFFFF", "#1F4E8C", "#FFFFFF",
            "#97C002", "#A7C928", "#2B4832", "#000000", "#FFFFFF",
            "#144F1C", /* was #8FE39A */ "#FFFFFF", /* was #111111 */ "#5E3F06", /* was #F5C36B */ "#FFFFFF", /* was #111111 */ "#003E9F", /* was #9DC3FF */ "#FFFFFF" /* was #111111 */),
    SINGLE("Wii Sports", "Claro",
           "#3FA3DE", "#5CB1E3", "#153454", "#000000", "#FFFFFF",
           "#113B1D", /* was #1E6B34 */ "#FFFFFF", "#4F2A00", /* was #8A4A00 */ "#FFFFFF", "#14335C", /* was #1F4E8C */ "#FFFFFF"),
    PALETTE("New Super Mario Bros. Wii",
            "#E4001C", "#C20018", "#F6F9FC", "#FFFFFF", "#000000",
            "#EEF9EE", /* was #2E7D32 */ "#000000", /* was #FFFFFF */ "#FDF6DF", /* was #F3C31A */ "#111111", "#F0F7FD", /* was #1C68B6 */ "#000000" /* was #FFFFFF */,
            "#185FAD", "#145193", "#C2DAFF", "#FFFFFF", "#000000",
            "#B6E4B8", /* was #2E7D32 */ "#000000", /* was #FFFFFF */ "#FFD52B", /* was #F5C400 */ "#111111", "#BDDCF8", /* was #6FB3F0 */ "#111111"),
    PALETTE("GameCube Indigo",
            "#49427B", "#3E3869", "#BBB4DB", "#FFFFFF", "#000000",
            "#54CF78", /* was #1E6B34 */ "#000000", /* was #FFFFFF */ "#FFA43A", /* was #8A4A00 */ "#000000", /* was #FFFFFF */ "#98BBE8", /* was #1F4E8C */ "#000000" /* was #FFFFFF */,
            "#49427B", "#3E3869", "#C0B2EF", "#FFFFFF", "#000000",
            "#8FE39A", "#111111", "#F5C36B", "#111111", "#9DC3FF", "#111111"),
    PALETTE("GameCube Spice",
            "#FDF6EF", "#FDF7F1", "#9A4A00", "#000000", "#FFFFFF",
            "#1E6B34", "#FFFFFF", "#8A4A00", "#FFFFFF", "#1F4E8C", "#FFFFFF",
            "#1E1410", "#1A110E", "#F08A3C", "#FFFFFF", "#000000",
            "#8FE39A", "#111111", "#F5C36B", "#111111", "#9DC3FF", "#111111"),
    SINGLE("GameCube Jet Black", "Oscuro",
           "#0F0F12", "#0D0D0F", "#7CB8FF", "#FFFFFF", "#000000",
           "#8FE39A", "#111111", "#F5C36B", "#111111", "#9DC3FF", "#111111"),
    SINGLE("Wii Blanco", "Claro",
           "#E3E3E3", "#E7E7E7", "#0A6BA1", "#000000", "#FFFFFF",
           "#1E6B34", "#FFFFFF", "#8A4A00", "#FFFFFF", "#1F4E8C", "#FFFFFF"),
    SINGLE("Wii Azul", "Oscuro",
           "#0B1A2A", "#091624", "#34BEED", "#FFFFFF", "#000000",
           "#8FE39A", "#111111", "#F5C36B", "#111111", "#9DC3FF", "#111111"),
};

/* Number of entries in the catalog. */
size_t palette_table_count(void)
{
    return sizeof(palette_table) / sizeof(palette_table[0]);
}

/* Returns entry i, or NULL if i is out of range. */
const palette_entry_t *palette_table_get(size_t i)
{
    if (i >= palette_table_count())
        return NULL;
    return &palette_table[i];
}
