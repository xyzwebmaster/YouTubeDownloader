#!/usr/bin/env python3
"""
Browser-driven TikTok uploader for YouTubeDownloader.

This script is the bridge that the C++ host (the .exe in this repo)
shells out to when the user picks "Browser mode" in the Upload tab.
It drives Chromium via Playwright using a *persistent* profile —
the first time you call `setup`, a visible browser opens for you to
log into TikTok manually; the cookies are saved under
%APPDATA%\\YouTubeDownloader\\playwright_profile and reused on every
subsequent call.

Subcommands and their stdout protocol (one JSON dict per line):

    setup         {"stage":"login_open","msg":"..."}, then {"ok":true,"stage":"done"}
    status        {"ok":true,"logged_in":bool,"username":str}
    upload        {"stage":"open"} {"stage":"uploading"} {"stage":"ready"}
                  {"ok":true,"stage":"posted"}  or  {"ok":false,"error":...}

Stderr is human-readable. The exit code is 0 on success, 1 on failure.

WARNING: TikTok actively detects automated uploads. This may trigger
rate limits, captchas, or account bans. Use a test / dedicated account
if you care about it.

Setup (one-time, in any terminal):
    pip install playwright
    python -m playwright install chromium
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
import traceback
from pathlib import Path


if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")


def emit(d: dict) -> None:
    """One JSON line per stdout write — that's how the C++ host reads us."""
    print(json.dumps(d, ensure_ascii=False), flush=True)


try:
    from playwright.sync_api import TimeoutError as PlaywrightTimeoutError
    from playwright.sync_api import sync_playwright
except ImportError:
    emit({
        "ok": False,
        "error": (
            "Playwright kurulu degil. Bir terminalde:\n"
            "    pip install playwright\n"
            "    python -m playwright install chromium"
        ),
    })
    sys.exit(1)


# Persistent Chromium profile location. Sharing data dir across runs
# is what lets the user log in once and stay logged in.
PROFILE_DIR = (
    Path(os.environ.get("APPDATA", str(Path.home())))
    / "YouTubeDownloader" / "playwright_profile"
)


def open_context(p, *, headless: bool):
    """Launch Chromium with the persistent profile.

    --disable-blink-features=AutomationControlled hides the
    `navigator.webdriver` flag that's the easiest tell. It does not
    defeat sophisticated detection — see playwright-stealth if this
    starts failing.
    """
    PROFILE_DIR.mkdir(parents=True, exist_ok=True)
    return p.chromium.launch_persistent_context(
        str(PROFILE_DIR),
        headless=headless,
        viewport={"width": 1280, "height": 900},
        args=["--disable-blink-features=AutomationControlled"],
    )


def _setup_page(ctx):
    try:
        if ctx.pages:
            return ctx.pages[0]
    except Exception:
        pass
    return ctx.new_page()


def _wait_for_setup_close(ctx, page) -> None:
    """Return when the user closes the setup tab/window."""
    while True:
        try:
            page.wait_for_event("close", timeout=1000)
            return
        except PlaywrightTimeoutError:
            pass
        except Exception:
            return

        try:
            if page.is_closed():
                return
            if not [p for p in ctx.pages if not p.is_closed()]:
                return
        except Exception:
            return


def cmd_setup(args) -> int:
    """Open visible browser, navigate to login, wait for the user to
    close the window. Anything they do (login, captcha) persists in
    PROFILE_DIR for subsequent `upload` calls."""
    with sync_playwright() as p:
        ctx = open_context(p, headless=False)
        page = _setup_page(ctx)
        try:
            page.goto("https://www.tiktok.com/login", timeout=60000)
        except Exception as e:
            emit({"ok": False, "error": f"login sayfasi acilamadi: {e}"})
            return 1
        emit({"stage": "login_open",
              "msg": "Acilan pencereden TikTok'a giris yap. "
                     "Pencereyi kapatinca kayit biter."})

        # Just wait until the user closes the only page (or the whole
        # context). We don't auto-detect login completion because the
        # user might want to verify their settings before closing.
        _wait_for_setup_close(ctx, page)
        emit({"ok": True, "stage": "done"})
        try:
            ctx.close()
        except Exception:
            pass
    return 0


def cmd_status(args) -> int:
    """Headless visit to detect login state and (best-effort) username."""
    with sync_playwright() as p:
        ctx = open_context(p, headless=True)
        try:
            page = ctx.new_page()
            page.goto("https://www.tiktok.com/", timeout=45000)
            time.sleep(2)
            logged_in = page.locator("[data-e2e='profile-icon']").count() > 0
            username = ""
            if logged_in:
                # The profile icon wraps an <a> whose href is /@username.
                try:
                    href = page.locator("[data-e2e='profile-icon'] a").first \
                                .get_attribute("href") or ""
                    if href.startswith("/@"):
                        username = href[2:].split("?")[0]
                except Exception:
                    pass
            emit({"ok": True, "logged_in": logged_in, "username": username})
        finally:
            try: ctx.close()
            except Exception: pass
    return 0


def _fill_caption(page, text: str) -> None:
    """Find the upload page's caption editor and replace its content.

    TikTok has used several different selectors for this over time;
    we try them in order and bail out at the first one that exists.
    """
    selectors = [
        "[data-e2e='caption-editor'] [contenteditable='true']",
        "div[role='combobox'][contenteditable='true']",
        "[contenteditable='true']",
    ]
    for sel in selectors:
        if page.locator(sel).count() > 0:
            editor = page.locator(sel).first
            editor.click()
            page.keyboard.press("Control+A")
            page.keyboard.press("Delete")
            page.keyboard.type(text, delay=15)
            return


POST_BUTTON_TIMEOUT_MS = 300000
POST_BUTTON_MARK = "data-ytdl-post-target"
POST_BUTTON_SELECTOR_MARK = "data-ytdl-post-selector"
POST_BUTTON_SELECTORS = [
    "[data-e2e='post_video_button']",
    "[data-e2e='post-button']",
    "[data-e2e='post_button']",
    "[data-testid='post_video_button']",
    "[data-testid='post-button']",
    "[data-testid='post_button']",
]


def _click_post(page):
    """Wait for TikTok's publish control to be actionable, then click it.

    The upload page is localized, so visible button labels are a poor
    contract. Prefer stable data hooks and explicit enabled/visible
    checks instead.
    """
    page.wait_for_function(
        """
        ({ selectors, targetMark, selectorMark }) => {
            const seen = new Set();

            const asClickable = (node) => {
                if (!(node instanceof Element)) return null;
                if (node.matches("button,[role='button']")) return node;
                return node.closest("button,[role='button']") ||
                    node.querySelector("button,[role='button']") ||
                    node;
            };

            const isUsable = (el) => {
                if (!(el instanceof HTMLElement)) return false;
                const rect = el.getBoundingClientRect();
                if (rect.width <= 0 || rect.height <= 0) return false;

                const style = window.getComputedStyle(el);
                if (style.display === "none" ||
                    style.visibility === "hidden" ||
                    style.pointerEvents === "none") {
                    return false;
                }

                if (el.matches(":disabled") ||
                    el.getAttribute("aria-disabled") === "true" ||
                    el.getAttribute("data-disabled") === "true" ||
                    el.closest("[aria-disabled='true']")) {
                    return false;
                }
                return true;
            };

            document
                .querySelectorAll("[" + targetMark + "='1']")
                .forEach((el) => {
                    el.removeAttribute(targetMark);
                    el.removeAttribute(selectorMark);
                });

            for (const selector of selectors) {
                for (const node of document.querySelectorAll(selector)) {
                    const button = asClickable(node);
                    if (!button || seen.has(button)) continue;
                    seen.add(button);
                    if (!isUsable(button)) continue;

                    button.setAttribute(targetMark, "1");
                    button.setAttribute(selectorMark, selector);
                    return true;
                }
            }
            return false;
        }
        """,
        arg={
            "selectors": POST_BUTTON_SELECTORS,
            "targetMark": POST_BUTTON_MARK,
            "selectorMark": POST_BUTTON_SELECTOR_MARK,
        },
        timeout=POST_BUTTON_TIMEOUT_MS,
    )

    button = page.locator(f"[{POST_BUTTON_MARK}='1']").first
    selector = button.get_attribute(POST_BUTTON_SELECTOR_MARK) or "data hook"
    button.scroll_into_view_if_needed(timeout=10000)
    button.click(timeout=10000)
    return selector


def _wait_for_post_success(page) -> bool:
    """Wait for a language-neutral success signal after publish."""
    try:
        page.wait_for_url("**/tiktokstudio/content**", timeout=180000)
        return True
    except Exception:
        pass

    try:
        page.wait_for_function(
            "() => location.pathname.includes('/tiktokstudio/content')",
            timeout=20000,
        )
        return True
    except Exception:
        return False


def cmd_upload(args) -> int:
    file_path = Path(args.file).resolve()
    if not file_path.is_file():
        emit({"ok": False, "error": f"dosya bulunamadi: {file_path}"})
        return 1

    with sync_playwright() as p:
        ctx = open_context(p, headless=args.headless)
        try:
            page = ctx.new_page()
            emit({"stage": "open"})
            page.goto(
                "https://www.tiktok.com/tiktokstudio/upload?from=upload",
                timeout=120000,
            )

            # The upload page renders the file input even before the user
            # clicks "Select video", so we can drive it directly.
            page.wait_for_selector("input[type='file']", timeout=60000)
            page.set_input_files("input[type='file']", str(file_path))
            emit({"stage": "uploading"})

            if args.caption:
                _fill_caption(page, args.caption)

            emit({"stage": "ready"})
            try:
                post_selector = _click_post(page)
            except Exception as e:
                emit({"ok": False, "error":
                      f"Post butonu bulunamadi / aktiflesmedi: {e}"})
                return 1
            emit({"stage": "post_clicked", "selector": post_selector})

            # Success indicator: TikTok redirects to the content list
            # once the post is queued. This stays stable across locales.
            if _wait_for_post_success(page):
                emit({"ok": True, "stage": "posted"})
                return 0
            emit({"ok": False, "stage": "post-wait",
                  "error": "3 dk icinde basari onayi alinamadi"})
            return 1
        except Exception as e:
            emit({"ok": False, "error": str(e),
                  "trace": traceback.format_exc().splitlines()[-3:]})
            return 1
        finally:
            try: ctx.close()
            except Exception: pass


HANDLERS = {"setup": cmd_setup, "status": cmd_status, "upload": cmd_upload}


def main() -> None:
    ap = argparse.ArgumentParser(
        description="Browser-driven TikTok uploader for YouTubeDownloader.")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("setup")
    sub.add_parser("status")
    up = sub.add_parser("upload")
    up.add_argument("--file", required=True)
    up.add_argument("--caption", default="")
    up.add_argument("--headless", action="store_true",
                    help="run without a visible window (more bot-like, "
                         "more likely to trip TikTok detection)")
    args = ap.parse_args()

    try:
        rc = HANDLERS[args.cmd](args) or 0
    except KeyboardInterrupt:
        emit({"ok": False, "error": "interrupted"})
        rc = 1
    except Exception as e:
        emit({"ok": False, "error": f"unhandled: {e}",
              "trace": traceback.format_exc().splitlines()[-3:]})
        rc = 1
    sys.exit(rc)


if __name__ == "__main__":
    main()
