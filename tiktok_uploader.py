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


def emit(d: dict) -> None:
    """One JSON line per stdout write — that's how the C++ host reads us."""
    print(json.dumps(d, ensure_ascii=False), flush=True)


try:
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


def cmd_setup(args) -> int:
    """Open visible browser, navigate to login, wait for the user to
    close the window. Anything they do (login, captcha) persists in
    PROFILE_DIR for subsequent `upload` calls."""
    with sync_playwright() as p:
        ctx = open_context(p, headless=False)
        page = ctx.new_page()
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
        while True:
            time.sleep(1)
            try:
                if not ctx.pages:
                    break
            except Exception:
                break
        emit({"ok": True, "stage": "done"})
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


def _click_post(page) -> bool:
    """Click whichever variant of the Post button is currently in the DOM.

    TikTok's button has been data-e2e='post_video_button' and
    'post-button' over time; localizations also change the visible
    text. Returns True if a click happened.
    """
    candidates = [
        "button[data-e2e='post_video_button']:not([disabled])",
        "button[data-e2e='post-button']:not([disabled])",
        "button:has-text('Post'):not([disabled])",
        "button:has-text('Yayinla'):not([disabled])",
        "button:has-text('Yayınla'):not([disabled])",
    ]
    for sel in candidates:
        try:
            page.wait_for_selector(sel, timeout=300000)
            page.click(sel)
            return True
        except Exception:
            continue
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
            if not _click_post(page):
                emit({"ok": False, "error":
                      "Post butonu bulunamadi / aktiflesmedi"})
                return 1

            # Success indicator: TikTok redirects to the content list
            # once the post is queued. As a fallback, look for a toast.
            try:
                page.wait_for_url("**/tiktokstudio/content**", timeout=180000)
                emit({"ok": True, "stage": "posted"})
                return 0
            except Exception:
                pass
            try:
                page.wait_for_selector(
                    "text=/posted|yayınlan|video uploaded/i",
                    timeout=20000,
                )
                emit({"ok": True, "stage": "posted"})
                return 0
            except Exception as e:
                emit({"ok": False, "stage": "post-wait",
                      "error":
                      f"3 dk icinde basari onayi alinamadi: {e}"})
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
