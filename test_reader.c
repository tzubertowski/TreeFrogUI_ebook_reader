/* Run: gcc -I../mupdf/include -ffunction-sections -fdata-sections test_reader.c
 * -Wl,--gc-sections -o /tmp/test-ebook-reader && /tmp/test-ebook-reader */
#define main reader_main
#include "reader.c"
#undef main
#include <assert.h>

int main(void) {
    char dir[] = "/tmp/ebook-check-XXXXXX";
    assert(mkdtemp(dir) && chdir(dir) == 0);
    FILE *f = fopen("book.epub.pos", "w");
    assert(f && fprintf(f, "7 26 1 2\n") > 0);
    assert(fclose(f) == 0);
    font_count = 3;
    pos_init("book.epub");
    pos_load();
    assert(page == 7 && font_px == 26 && font_sel == 1 && theme == TH_DARK);
    assert(access("book.epub.pos", F_OK) != 0);
    page = 9;
    pos_mark_dirty();
    pos_save();
    assert(!pos_dirty);
    page = 0;
    pos_load();
    assert(page == 9 && theme == TH_DARK);
    strcpy(pos_path, "missing/book.pos");
    pos_mark_dirty();
    pos_save();
    assert(pos_dirty); /* Failed saves must remain pending. */

    PW = 4;
    PH = STATUS_BAR_HEIGHT + 2;
    FB = calloc((size_t)PW * PH, sizeof(*FB));
    assert(FB);
    unsigned char samples[] = {0, 0, 0, 0, 0, 0};
    fz_pixmap pix = {0};
    pix.w = 2; pix.h = 1; pix.n = 3; pix.stride = 6; pix.samples = samples;
    theme = TH_LIGHT;
    pix_to_fb(&pix);
    assert(FB[0] == 0xffff && FB[1] == 0 && FB[2] == 0 && FB[3] == 0xffff);
    assert(FB[PW * reading_height()] == 0xffff);
    free(FB);
    puts("Reader save/migration and page placement checks passed.");
    return 0;
}
