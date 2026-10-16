/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "model.h"
#include "browser.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    GuiMenu menu = {0};
    GuiMenu ring;
    GuiConfirm confirm = {0};
    /* no games: Games is hidden, and the ring goes past it */
    menu.selected = GUI_SWITCH;
    gui_menu_key(&menu, GUI_LEFT);
    assert(menu.selected == GUI_EXIT);
    gui_menu_key(&menu, GUI_RIGHT);
    assert(menu.selected == GUI_SWITCH && !menu.activated);
    /* a load opens its screen at once; ESC goes back to the menu, on it */
    gui_menu_key(&menu, GUI_SELECT);
    assert(menu.view == GUI_LOAD_DROP && menu.activated && menu.active == GUI_SWITCH && !menu.running);
    gui_menu_key(&menu, GUI_CANCEL);
    assert(menu.view == GUI_LOAD_NONE && menu.selected == GUI_SWITCH);
    /* with games, Games comes first and opens their browser, which takes the keys */
    menu.games = true;
    gui_menu_key(&menu, GUI_UP);
    assert(menu.selected == GUI_GAMES);
    assert(!strcmp(gui_setting_description(&menu), "The games of games/ and of the folders added in Settings > App"));
    gui_menu_key(&menu, GUI_SELECT);
    assert(menu.view == GUI_LOAD_GAMES && menu.active == GUI_GAMES);
    assert(gui_menu_key(&menu, GUI_DOWN) == GUI_MENU_STAY && menu.selected == GUI_GAMES);
    menu.view = GUI_LOAD_NONE; /* Back from the browser */
    gui_menu_key(&menu, GUI_UP);
    assert(menu.selected == GUI_EXIT);
    gui_menu_key(&menu, GUI_DOWN);
    assert(menu.selected == GUI_GAMES);
    /* the games gone while Games had the focus: the first load takes it */
    menu.games = false;
    gui_menu_key(&menu, GUI_DOWN);
    assert(menu.selected == GUI_LINUX);
    gui_menu_key(&menu, GUI_DOWN);
    assert(menu.selected == GUI_3DS);
    gui_menu_key(&menu, GUI_SELECT);
    assert(menu.active == GUI_3DS && menu.view == GUI_LOAD_DROP);
    gui_menu_key(&menu, GUI_CANCEL);
    gui_menu_key(&menu, GUI_RIGHT);
    assert(menu.selected == GUI_KEYS);
    gui_menu_key(&menu, GUI_SELECT);
    assert(menu.view == GUI_LOAD_KEYS && !menu.running);
    gui_menu_key(&menu, GUI_DOWN);
    gui_menu_key(&menu, GUI_SELECT);
    assert(menu.selected == GUI_KEYS && menu.view == GUI_LOAD_KEYS);
    gui_menu_key(&menu, GUI_CANCEL);
    assert(menu.view == GUI_LOAD_NONE && !menu.quit);
    gui_menu_key(&menu, GUI_RIGHT);
    assert(menu.selected == GUI_SETTINGS);
    /* Settings opens the screens of the game's settings, the launcher's and its App (settings.h, gui_settings.c) */
    assert(gui_menu_key(&menu, GUI_SELECT) == GUI_MENU_SETTINGS && menu.active == GUI_SETTINGS);
    assert(menu.view == GUI_LOAD_NONE && strstr(gui_setting_description(&menu), "its theme"));
    menu.running = true;
    assert(gui_menu_key(&menu, GUI_DOWN) == GUI_MENU_STAY && menu.selected == GUI_SETTINGS);
    assert(gui_menu_key(&menu, GUI_SELECT) == GUI_MENU_STAY);
    menu.running = false;
    gui_menu_key(&menu, GUI_DOWN);
    assert(gui_menu_key(&menu, GUI_SELECT) == GUI_MENU_LOG && menu.selected == GUI_LOG && !menu.quit);
    gui_menu_key(&menu, GUI_DOWN);
    gui_menu_key(&menu, GUI_SELECT);
    assert(menu.quit);

    /* a load's screen keeps the load a picked file goes to while the focus moves (a phone's menu shows under it) */
    menu = (GuiMenu){0};
    gui_menu_key(&menu, GUI_SELECT); /* Games hidden: the first load */
    assert(menu.active == GUI_SWITCH && menu.view == GUI_LOAD_DROP);
    menu.selected = GUI_LINUX;
    gui_menu_key(&menu, GUI_DOWN);
    assert(menu.selected == GUI_3DS && menu.active == GUI_SWITCH && menu.view == GUI_LOAD_DROP);
    gui_menu_key(&menu, GUI_CANCEL);
    assert(menu.view == GUI_LOAD_NONE && menu.selected == GUI_SWITCH);

    /* the footer's use, as a player reads it: MB under a GB, the CPU a share of every core, never over 100% */
    {
        char use[128];
        gui_usage_text(use, sizeof use, true, 812ull << 20, 52, 4);
        assert(!strcmp(use, "GUI + children: RAM 812 MB  CPU 13% (0.5 cores)"));
        gui_usage_text(use, sizeof use, true, 1404ull << 20, 216, 8); /* a game compiling on 8 cores */
        assert(!strcmp(use, "GUI + children: RAM 1.4 GB  CPU 27% (2.2 cores)"));
        gui_usage_text(use, sizeof use, true, 1, 830, 8);
        assert(!strcmp(use, "GUI + children: RAM 0 MB  CPU 100% (8.0 cores)"));
        gui_usage_text(use, sizeof use, true, 0, 150, 0); /* SDL knows no cores: one */
        assert(!strcmp(use, "GUI + children: RAM 0 MB  CPU 100% (1.0 cores)"));
        gui_usage_text(use, sizeof use, false, 1, 1, 1);
        assert(!strcmp(use, "GUI + children: resource usage unavailable"));
        /* sizes, as the footer and the Settings' disk line write them: MB under a GB */
        gui_size_text(use, sizeof use, 0);
        assert(!strcmp(use, "0 MB"));
        gui_size_text(use, sizeof use, 1023ull << 20);
        assert(!strcmp(use, "1023 MB"));
        gui_size_text(use, sizeof use, 1ull << 30);
        assert(!strcmp(use, "1.0 GB"));
        gui_size_text(use, sizeof use, 26ull << 30 | 400ull << 20);
        assert(!strcmp(use, "26.4 GB"));
    }
    /* a line 5 columns too long, a column every 100 ms: rests 8 steps, slides, rests at its end, slides back */
    assert(!gui_slide(0, 900, 100) && !gui_slide(5, 900, 0) && !gui_slide(-3, 900, 100));
    assert(!gui_slide(5, 0, 100) && !gui_slide(5, 799, 100) && gui_slide(5, 900, 100) == 1);
    assert(gui_slide(5, 1300, 100) == 5 && gui_slide(5, 2000, 100) == 5 && gui_slide(5, 2200, 100) == 4);
    assert(gui_slide(5, 2500, 100) == 1 && !gui_slide(5, 2600, 100) && gui_slide(5, 2600 + 900, 100) == 1);

    /* a row of a wrapped line: whole when it fits, else after its last slash, else its last space, else cut */
    assert(gui_wrap("abc", 5) == 3 && gui_wrap("abcde", 5) == 5 && gui_wrap("", 5) == 0);
    assert(gui_wrap("/home/me/games/Super Mario Maker 2.ini", 20) == 15); /* "/home/me/games/" */
    assert(gui_wrap("C:\\Users\\me\\Super Mario", 16) == 12);             /* up to the last backslash */
    assert(gui_wrap("An older version left", 10) == 9);                   /* "An older " */
    assert(gui_wrap("abc/", 4) == 4 && gui_wrap("abc/def", 4) == 4 && gui_wrap("abcdefghij", 4) == 4);
    assert(gui_wrap("ab", 0) == 1 && gui_wrap("ab", -3) == 1);
    assert(gui_wrap("\xc3\xa9\xc3\xa9\xc3\xa9", 2) == 4); /* characters, not bytes */

    GuiBrowser browser = {.count = 23};
    gui_browser_resize(&browser, 10);
    assert(gui_browser_pages(&browser) == 3 && gui_browser_items(&browser) == 10);
    assert(gui_browser_key(&browser, GUI_SELECT) == GUI_BROWSER_BACK);
    gui_browser_key(&browser, GUI_UP);
    assert(browser.selected == 10 && !browser.activated);
    gui_browser_key(&browser, GUI_RIGHT);
    assert(browser.page == 1 && browser.selected == 10);
    gui_browser_key(&browser, GUI_PAGE_DOWN);
    assert(browser.page == 2 && browser.selected == 3 && gui_browser_items(&browser) == 3);
    gui_browser_key(&browser, GUI_RIGHT); /* no wrapping past the last page */
    assert(browser.page == 2);
    assert(gui_browser_key(&browser, GUI_SELECT) == GUI_BROWSER_OPEN);
    assert(browser.activated && browser.active == 22);
    gui_browser_resize(&browser, 7);
    assert(browser.page * browser.page_size + browser.selected - 1 == 22);
    gui_browser_key(&browser, GUI_PAGE_UP);
    gui_browser_key(&browser, GUI_LEFT);
    assert(browser.page == 1);
    assert(gui_browser_key(&browser, GUI_CANCEL) == GUI_BROWSER_BACK);
    browser = (GuiBrowser){0};
    gui_browser_resize(&browser, 0);
    gui_browser_key(&browser, GUI_DOWN);
    gui_browser_key(&browser, GUI_PAGE_DOWN);
    assert(browser.page == 0 && browser.selected == 0 && gui_browser_pages(&browser) == 1);

    GuiConsole screen;
    gui_console_init(&screen, 5, 3);
    gui_console_feed(&screen, "abcdefghijklmnoP", 16);
    assert(!memcmp(screen.cells[0], "fghij", 5));
    assert(!memcmp(screen.cells[1], "klmno", 5));
    assert(screen.cells[2][0] == 'P');
    gui_console_feed(&screen, "\rX\n", 3);
    assert(screen.cells[1][0] == 'X' && screen.col == 0);
    gui_console_init(&screen, 10, 2);
    gui_console_feed(&screen, "\033[3", 3);
    gui_console_feed(&screen, "1mred\033[0m\033]0;title\007!", strlen("1mred\033[0m\033]0;title\007!"));
    assert(!memcmp(screen.cells[0], "red!", 4));
    gui_console_feed(&screen, "\xc3", 1);
    gui_console_feed(&screen, "\xa9", 1);
    assert(screen.cells[0][4] == gui_glyph(0xe9));
    /* the font's symbols under 32 and its house at 127 come from their Unicode, as a theme's cursor or a key's arrow;
       the control bytes stay themselves */
    assert(gui_glyph(0x25ba) == 0x10 && gui_glyph(0x2022) == 0x07 && gui_glyph(0x25ac) == 0x16);
    assert(gui_glyph(0x2191) == 0x18 && gui_glyph(0x2193) == 0x19 && gui_glyph(0x2192) == 0x1a);
    assert(gui_glyph(0x2190) == 0x1b && gui_glyph(0x263a) == 0x01 && gui_glyph(0x25bc) == 0x1f);
    assert(gui_glyph(0x2302) == 0x7f && gui_glyph(0x00a7) == 0x15 && gui_glyph(0x10) == 0x10);
    for (int i = 0; i < 1000000; ++i) gui_console_feed(&screen, "z", 1);
    assert(screen.row < screen.rows && screen.col <= screen.cols);
    gui_console_resize(&screen, 3, 1);
    assert(screen.row == 0 && screen.cols == 3 && screen.col <= 3);
    gui_console_resize(&screen, 9999, 9999);
    assert(screen.rows == GUI_MAX_ROWS && screen.cols == GUI_MAX_COLS);

    char* path = gui_drop_path("  \"/tmp/a b;$(not-a-shell).nro\"\r\n", true);
    assert(path && !strcmp(path, "/tmp/a b;$(not-a-shell).nro"));
    free(path);
    path = gui_drop_path("file://localhost/tmp/a%20b.nro", true);
    assert(path && !strcmp(path, "/tmp/a b.nro"));
    free(path);
    path = gui_drop_path(" /a real trailing space ", false);
    assert(path && !strcmp(path, " /a real trailing space "));
    free(path);
    assert(!gui_drop_path("file://remote/tmp/a", true));
    assert(!gui_drop_path("file:///tmp/%00.nro", true));
    assert(!gui_drop_path("file:///tmp/%zz", true));
    assert(!gui_drop_path("one\ntwo", true));
    assert(!gui_drop_path("", true));
    /* what a picker gives: the Android one's documents of the device's storage and its cards, a download by path; no
       other of its documents has a path */
    path =
        gui_document_path("content://com.android.externalstorage.documents/document/primary%3ADownload%2FGame%20X.xci");
    assert(path && !strcmp(path, "/storage/emulated/0/Download/Game X.xci"));
    free(path);
    path = gui_document_path("content://com.android.externalstorage.documents/document/1A2B-3C4D%3Agames%2Fa.nsp");
    assert(path && !strcmp(path, "/storage/1A2B-3C4D/games/a.nsp"));
    free(path);
    path = gui_document_path("content://com.android.providers.downloads.documents/document/"
                             "raw%3A%2Fstorage%2Femulated%2F0%2FDownload%2Fprod.keys");
    assert(path && !strcmp(path, "/storage/emulated/0/Download/prod.keys"));
    free(path);
    assert(!gui_document_path("content://com.android.providers.downloads.documents/document/msf%3A1234"));
    assert(!gui_document_path("content://com.android.providers.media.documents/document/document%3A12"));
    assert(!gui_document_path("content://com.android.externalstorage.documents/document/primary%3A%zz"));
    assert(!gui_document_path("content://com.android.externalstorage.documents/document/primary%3A%00.xci"));
    assert(!gui_document_path("content://com.android.externalstorage.documents/document/primary"));
    /* a folder its folder picker gave: a tree of the device's storage, its root too, or a card's; Downloads' has none */
    path = gui_document_path("content://com.android.externalstorage.documents/tree/primary%3AGames%2FSwitch");
    assert(path && !strcmp(path, "/storage/emulated/0/Games/Switch"));
    free(path);
    path = gui_document_path("content://com.android.externalstorage.documents/tree/primary%3A");
    assert(path && !strcmp(path, "/storage/emulated/0/"));
    free(path);
    path = gui_document_path("content://com.android.externalstorage.documents/tree/1A2B-3C4D%3Agames");
    assert(path && !strcmp(path, "/storage/1A2B-3C4D/games"));
    free(path);
    assert(!gui_document_path("content://com.android.providers.downloads.documents/tree/downloads"));
    path = gui_document_path("file:///tmp/a%20b.nro");
    assert(path && !strcmp(path, "/tmp/a b.nro"));
    free(path);
    path = gui_document_path("/tmp/ a.nro ");
    assert(path && !strcmp(path, "/tmp/ a.nro "));
    free(path);
    /* the dumps the Android games/ list offers, to prepare first */
    assert(gui_is_dump("a.NRO") && gui_is_dump("b.xci") && gui_is_dump("c.nsp") && gui_is_dump("d.dxci"));
    assert(!gui_is_dump("e_code.so") && !gui_is_dump("nro") && !gui_is_dump("f.nro.ini"));
    path = gui_document_path("C:\\Games\\a.nro"); /* Windows' */
    assert(path && !strcmp(path, "C:\\Games\\a.nro"));
    free(path);
    /* two recent games and the items make one ring; selecting a game asks the GUI to run it */
    memset(&ring, 0, sizeof ring);
    ring.recent = 2;
    ring.selected = GUI_SWITCH;
    gui_menu_key(&ring, GUI_UP);
    assert(ring.recent_focus == 2 && ring.selected == GUI_SWITCH);
    gui_menu_key(&ring, GUI_LEFT);
    assert(ring.recent_focus == 1);
    gui_menu_key(&ring, GUI_UP);
    assert(!ring.recent_focus && ring.selected == GUI_EXIT);
    gui_menu_key(&ring, GUI_DOWN);
    assert(ring.recent_focus == 1);
    gui_menu_key(&ring, GUI_RIGHT);
    assert(ring.recent_focus == 2);
    gui_menu_key(&ring, GUI_DOWN);
    assert(!ring.recent_focus && ring.selected == GUI_SWITCH);
    ring.games = true; /* with games, the recent rows lead to Games */
    ring.recent_focus = 2;
    gui_menu_key(&ring, GUI_DOWN);
    assert(!ring.recent_focus && ring.selected == GUI_GAMES);
    gui_menu_key(&ring, GUI_UP);
    assert(ring.recent_focus == 2);
    ring.view = GUI_LOAD_DROP;
    assert(gui_menu_key(&ring, GUI_SELECT) == GUI_MENU_RECENT && ring.recent_focus == 2);
    assert(ring.view == GUI_LOAD_NONE && !ring.activated);
    ring.recent = 0;
    ring.recent_focus = 0;
    ring.games = false;
    ring.selected = GUI_SWITCH;
    gui_menu_key(&ring, GUI_UP); /* none: the items alone, as before */
    assert(ring.selected == GUI_EXIT && !ring.recent_focus);

    /* the question before an abort: keys do nothing while it is not shown; the arrows move between its choices, Select
       takes the one in focus and Cancel keeps going, both closing it */
    assert(!gui_confirm_key(&confirm, GUI_SELECT) && !gui_confirm_key(&confirm, GUI_DOWN));
    assert(!confirm.shown && confirm.selected == GUI_CONFIRM_KEEP);
    confirm.shown = true;
    assert(!gui_confirm_key(&confirm, GUI_SELECT) && !confirm.shown); /* Keep going is the first choice */
    confirm.shown = true;
    assert(!gui_confirm_key(&confirm, GUI_DOWN) && confirm.selected == GUI_CONFIRM_ABORT && confirm.shown);
    assert(!gui_confirm_key(&confirm, GUI_PAGE_DOWN) && confirm.selected == GUI_CONFIRM_ABORT && confirm.shown);
    assert(!gui_confirm_key(&confirm, GUI_CANCEL) && !confirm.shown); /* Back again, on Abort too, keeps going */
    confirm.shown = true;
    assert(!gui_confirm_key(&confirm, GUI_UP) && confirm.selected == GUI_CONFIRM_KEEP);
    assert(!gui_confirm_key(&confirm, GUI_RIGHT) && confirm.selected == GUI_CONFIRM_ABORT);
    assert(!gui_confirm_key(&confirm, GUI_LEFT) && confirm.selected == GUI_CONFIRM_KEEP);
    assert(!gui_confirm_key(&confirm, GUI_UP) && confirm.selected == GUI_CONFIRM_ABORT);
    assert(gui_confirm_key(&confirm, GUI_SELECT) && !confirm.shown);
    assert(!gui_confirm_key(&confirm, GUI_SELECT)); /* answered: nothing more to abort */
    puts("GUI menu, bounded console, UTF-8 and file drops passed");
    return 0;
}
