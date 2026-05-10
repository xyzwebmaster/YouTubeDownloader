#!/usr/bin/env python3
"""
Browser-driven Instagram uploader for YouTubeDownloader.

The C++ host shells out to this helper for Instagram browser uploads.
It uses a separate persistent Chromium profile so the user can log into
Instagram once and reuse that session for bulk uploads.
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


PROFILE_DIR = (
    Path(os.environ.get("APPDATA", str(Path.home())))
    / "YouTubeDownloader" / "instagram_playwright_profile"
)

ACTION_MARK = "data-ytdl-instagram-action"
ACTION_KIND_MARK = "data-ytdl-instagram-action-kind"
CAPTION_MARK = "data-ytdl-instagram-caption"


def open_context(p, *, headless: bool):
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
    with sync_playwright() as p:
        ctx = open_context(p, headless=False)
        page = _setup_page(ctx)
        try:
            page.goto("https://www.instagram.com/accounts/login/", timeout=60000)
        except Exception as e:
            emit({"ok": False, "error": f"Instagram login sayfasi acilamadi: {e}"})
            return 1
        emit({
            "stage": "login_open",
            "msg": (
                "Acilan pencereden Instagram'a giris yap. "
                "Pencereyi kapatinca kayit biter."
            ),
        })

        _wait_for_setup_close(ctx, page)
        emit({"ok": True, "stage": "done"})
        try:
            ctx.close()
        except Exception:
            pass
    return 0


def _visible_file_input(page):
    page.wait_for_selector("input[type='file']", state="attached", timeout=60000)
    return page.locator("input[type='file']").last


def _open_create_flow(page) -> None:
    page.goto("https://www.instagram.com/", timeout=120000)
    page.wait_for_timeout(1500)
    for _ in range(2):
        try:
            page.keyboard.press("Escape")
            page.wait_for_timeout(400)
        except Exception:
            pass

    try:
        page.goto("https://www.instagram.com/create/reel/", timeout=120000)
        _visible_file_input(page)
        return
    except Exception as e:
        raise RuntimeError(f"Instagram Reels create flow acilamadi: {e}")


def _wait_for_media_ready(page) -> None:
    page.wait_for_function(
        """
        () => {
            const visible = (el) => {
                if (!(el instanceof HTMLElement)) return false;
                const rect = el.getBoundingClientRect();
                const style = window.getComputedStyle(el);
                return rect.width > 0 && rect.height > 0 &&
                    style.display !== "none" &&
                    style.visibility !== "hidden";
            };
            return Array
                .from(document.querySelectorAll("[role='dialog'] video, [role='dialog'] img, [role='dialog'] canvas"))
                .some(visible);
        }
        """,
        timeout=120000,
    )


def _mark_caption_editor(page) -> bool:
    try:
        page.wait_for_function(
            """
            ({ captionMark }) => {
                const dialogs = Array
                    .from(document.querySelectorAll("[role='dialog']"))
                    .filter((el) => {
                        const rect = el.getBoundingClientRect();
                        const style = window.getComputedStyle(el);
                        return rect.width > 0 && rect.height > 0 &&
                            style.display !== "none" &&
                            style.visibility !== "hidden";
                    });
                const root = dialogs.at(-1) || document.body;

                root
                    .querySelectorAll("[" + captionMark + "='1']")
                    .forEach((el) => el.removeAttribute(captionMark));

                const candidates = [
                    ...root.querySelectorAll("textarea"),
                    ...root.querySelectorAll("[contenteditable='true'][role='textbox']"),
                    ...root.querySelectorAll("[contenteditable='true']"),
                ];

                for (const el of candidates) {
                    if (!(el instanceof HTMLElement)) continue;
                    const rect = el.getBoundingClientRect();
                    const style = window.getComputedStyle(el);
                    if (rect.width <= 0 || rect.height <= 0) continue;
                    if (style.display === "none" ||
                        style.visibility === "hidden" ||
                        style.pointerEvents === "none") {
                        continue;
                    }
                    if (el.matches(":disabled") ||
                        el.getAttribute("aria-disabled") === "true") {
                        continue;
                    }
                    el.setAttribute(captionMark, "1");
                    return true;
                }
                return false;
            }
            """,
            {"captionMark": CAPTION_MARK},
            timeout=5000,
        )
        return True
    except Exception:
        return False


def _fill_caption(page, caption: str) -> None:
    if not _mark_caption_editor(page):
        raise RuntimeError("Instagram aciklama alani bulunamadi")

    editor = page.locator(f"[{CAPTION_MARK}='1']").first
    tag = (editor.evaluate("(el) => el.tagName") or "").lower()
    if tag == "textarea":
        editor.fill(caption)
        return

    editor.click(timeout=10000)
    page.keyboard.press("Control+A")
    page.keyboard.press("Delete")
    if caption:
        page.keyboard.insert_text(caption)


def _click_primary_dialog_action(page, kind: str, *, timeout: int = 90000) -> str:
    page.wait_for_function(
        """
        ({ actionMark, kindMark, kind }) => {
            const isVisible = (el) => {
                if (!(el instanceof HTMLElement)) return false;
                const rect = el.getBoundingClientRect();
                const style = window.getComputedStyle(el);
                return rect.width > 0 && rect.height > 0 &&
                    style.display !== "none" &&
                    style.visibility !== "hidden" &&
                    style.pointerEvents !== "none";
            };

            const isEnabled = (el) => {
                return !el.matches(":disabled") &&
                    el.getAttribute("aria-disabled") !== "true" &&
                    el.closest("[aria-disabled='true']") === null;
            };

            const dialogs = Array
                .from(document.querySelectorAll("[role='dialog']"))
                .filter(isVisible)
                .sort((a, b) => {
                    const ar = a.getBoundingClientRect();
                    const br = b.getBoundingClientRect();
                    return (ar.width * ar.height) - (br.width * br.height);
                });
            const root = dialogs.at(-1) || document.body;
            const rootRect = root.getBoundingClientRect();

            document
                .querySelectorAll("[" + actionMark + "='1']")
                .forEach((el) => {
                    el.removeAttribute(actionMark);
                    el.removeAttribute(kindMark);
                });

            const candidates = Array
                .from(root.querySelectorAll("button,[role='button'],[tabindex='0']"))
                .filter((el) => {
                    if (!isVisible(el) || !isEnabled(el)) return false;
                    const text = (el.textContent || "").trim();
                    if (!text || text.length > 40) return false;
                    const rect = el.getBoundingClientRect();
                    return rect.top <= rootRect.top + 100 &&
                        rect.left >= rootRect.left + (rootRect.width * 0.45);
                })
                .map((el) => {
                    const rect = el.getBoundingClientRect();
                    const color = window.getComputedStyle(el).color;
                    const parts = (color.match(/\\d+(\\.\\d+)?/g) || [])
                        .map(Number);
                    const blue = parts.length >= 3 &&
                        parts[2] > 120 && parts[0] < 120;
                    return {
                        el,
                        score: (blue ? 10000 : 0) + rect.left - (rect.top * 0.2),
                    };
                })
                .sort((a, b) => b.score - a.score);

            if (!candidates.length) return false;
            candidates[0].el.setAttribute(actionMark, "1");
            candidates[0].el.setAttribute(kindMark, kind);
            return true;
        }
        """,
        {"actionMark": ACTION_MARK, "kindMark": ACTION_KIND_MARK, "kind": kind},
        timeout=timeout,
    )

    action = page.locator(f"[{ACTION_MARK}='1']").first
    label = action.evaluate("(el) => (el.textContent || '').trim()") or kind
    action.scroll_into_view_if_needed(timeout=10000)
    action.click(timeout=10000)
    return label


def _advance_to_caption(page) -> None:
    for idx in range(4):
        if _mark_caption_editor(page):
            return
        label = _click_primary_dialog_action(page, f"advance_{idx + 1}")
        emit({"stage": "advance", "selector": label})
        page.wait_for_timeout(2500)
    if not _mark_caption_editor(page):
        raise RuntimeError("Instagram aciklama adimina gecilemedi")


def _wait_for_post_success(page) -> bool:
    try:
        page.wait_for_function(
            """
            () => {
                if (!location.pathname.includes("/create")) return true;

                const visible = (el) => {
                    if (!(el instanceof HTMLElement)) return false;
                    const rect = el.getBoundingClientRect();
                    const style = window.getComputedStyle(el);
                    return rect.width > 0 && rect.height > 0 &&
                        style.display !== "none" &&
                        style.visibility !== "hidden";
                };

                const dialog = Array
                    .from(document.querySelectorAll("[role='dialog']"))
                    .filter(visible)
                    .at(-1);
                return !dialog;
            }
            """,
            timeout=180000,
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
            _open_create_flow(page)

            emit({"stage": "uploading"})
            _visible_file_input(page).set_input_files(str(file_path))
            _wait_for_media_ready(page)

            emit({"stage": "ready"})
            _advance_to_caption(page)
            _fill_caption(page, args.caption)

            label = _click_primary_dialog_action(page, "share")
            emit({"stage": "share_clicked", "selector": label})

            if _wait_for_post_success(page):
                emit({"ok": True, "stage": "posted"})
                return 0

            emit({
                "ok": False,
                "stage": "post-wait",
                "error": "3 dk icinde Instagram basari onayi alinamadi",
            })
            return 1
        except Exception as e:
            emit({
                "ok": False,
                "error": str(e),
                "trace": traceback.format_exc().splitlines()[-3:],
            })
            return 1
        finally:
            try:
                ctx.close()
            except Exception:
                pass


HANDLERS = {"setup": cmd_setup, "upload": cmd_upload}


def main() -> None:
    ap = argparse.ArgumentParser(
        description="Browser-driven Instagram uploader for YouTubeDownloader.")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("setup")
    up = sub.add_parser("upload")
    up.add_argument("--file", required=True)
    up.add_argument("--caption", default="")
    up.add_argument("--headless", action="store_true")
    args = ap.parse_args()

    try:
        rc = HANDLERS[args.cmd](args) or 0
    except KeyboardInterrupt:
        emit({"ok": False, "error": "interrupted"})
        rc = 1
    except Exception as e:
        emit({
            "ok": False,
            "error": f"unhandled: {e}",
            "trace": traceback.format_exc().splitlines()[-3:],
        })
        rc = 1
    sys.exit(rc)


if __name__ == "__main__":
    main()
