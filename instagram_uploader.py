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

INSTAGRAM_CAPTION_LIMIT = 2200


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
            "Playwright is not installed. In a terminal, run:\n"
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
CREATE_MARK = "data-ytdl-instagram-create"
FILE_INPUT_MARK = "data-ytdl-instagram-file"
DONE_MARK = "data-ytdl-instagram-done"


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
            emit({"ok": False, "error": f"Instagram login page could not be opened: {e}"})
            return 1
        emit({
            "stage": "login_open",
            "msg": (
                "Sign in to Instagram from the browser window. "
                "Close the window when you are done to save the session."
            ),
        })

        _wait_for_setup_close(ctx, page)
        emit({"ok": True, "stage": "done"})
        try:
            ctx.close()
        except Exception:
            pass
    return 0


def _mark_upload_file_input(page, *, timeout: int = 60000) -> None:
    page.wait_for_function(
        """
        ({ fileInputMark }) => {
            const isVisible = (el) => {
                if (!(el instanceof HTMLElement)) return false;
                const rect = el.getBoundingClientRect();
                const style = window.getComputedStyle(el);
                return rect.width > 0 && rect.height > 0 &&
                    style.display !== "none" &&
                    style.visibility !== "hidden";
            };

            document
                .querySelectorAll("[" + fileInputMark + "='1']")
                .forEach((el) => el.removeAttribute(fileInputMark));

            const dialogs = Array
                .from(document.querySelectorAll("[role='dialog']"))
                .filter(isVisible);
            for (const dialog of dialogs.reverse()) {
                const inputs = Array
                    .from(dialog.querySelectorAll("input[type='file']"))
                    .filter((input) => !input.disabled);
                if (!inputs.length) continue;
                inputs.at(-1).setAttribute(fileInputMark, "1");
                return true;
            }
            return false;
        }
        """,
        arg={"fileInputMark": FILE_INPUT_MARK},
        timeout=timeout,
    )


def _upload_file_input(page):
    _mark_upload_file_input(page)
    return page.locator(f"[{FILE_INPUT_MARK}='1']").first


def _click_create_button(page) -> str:
    page.wait_for_function(
        """
        ({ createMark }) => {
            const isVisible = (el) => {
                if (!(el instanceof HTMLElement)) return false;
                const rect = el.getBoundingClientRect();
                const style = window.getComputedStyle(el);
                return rect.width > 0 && rect.height > 0 &&
                    style.display !== "none" &&
                    style.visibility !== "hidden" &&
                    style.pointerEvents !== "none";
            };

            const labelOf = (el) => {
                const own = [
                    el.getAttribute("aria-label"),
                    el.getAttribute("title"),
                    el.textContent,
                    ...Array
                        .from(el.querySelectorAll("[aria-label],title"))
                        .map((child) => child.getAttribute("aria-label") || child.textContent),
                ];
                return own.filter(Boolean).join(" ").trim();
            };

            document
                .querySelectorAll("[" + createMark + "='1']")
                .forEach((el) => el.removeAttribute(createMark));

            const words = /create|new post|oluştur|olustur|paylaş|paylas/i;
            const candidates = Array
                .from(document.querySelectorAll("a,button,[role='button'],[tabindex='0']"))
                .filter((el) => {
                    if (!isVisible(el)) return false;
                    if (el.matches(":disabled") ||
                        el.getAttribute("aria-disabled") === "true") {
                        return false;
                    }
                    const label = labelOf(el);
                    const href = el.getAttribute("href") || "";
                    const rect = el.getBoundingClientRect();
                    const inLeftNav = rect.left < 140 && rect.width >= 24 &&
                        rect.height >= 24 && rect.top > 80;

                    if (href.includes("/create") && inLeftNav) return true;
                    if (words.test(label)) return true;

                    const hasPlusLikeSvg = !!el.querySelector(
                        "svg[aria-label*='New'],svg[aria-label*='Create'],svg[aria-label*='Olu'],svg[aria-label*='Pay']"
                    );
                    return inLeftNav && hasPlusLikeSvg;
                })
                .map((el) => {
                    const rect = el.getBoundingClientRect();
                    const label = labelOf(el);
                    const href = el.getAttribute("href") || "";
                    let score = 0;
                    if (rect.left < 140) score += 10000;
                    if (words.test(label)) score += 5000;
                    if (href.includes("/create")) score += 1000;
                    return { el, score };
                })
                .sort((a, b) => b.score - a.score);

            if (!candidates.length) return false;
            candidates[0].el.setAttribute(createMark, "1");
            candidates[0].el.setAttribute(
                "data-ytdl-instagram-create-label",
                labelOf(candidates[0].el) || "create"
            );
            return true;
        }
        """,
        arg={"createMark": CREATE_MARK},
        timeout=30000,
    )

    create = page.locator(f"[{CREATE_MARK}='1']").first
    label = create.get_attribute("data-ytdl-instagram-create-label") or "create"
    create.click(timeout=10000)
    return label


def _click_create_menu_item(page) -> str:
    page.wait_for_function(
        """
        ({ createMark }) => {
            const isVisible = (el) => {
                if (!(el instanceof HTMLElement)) return false;
                const rect = el.getBoundingClientRect();
                const style = window.getComputedStyle(el);
                return rect.width > 0 && rect.height > 0 &&
                    style.display !== "none" &&
                    style.visibility !== "hidden" &&
                    style.pointerEvents !== "none";
            };

            const labelOf = (el) => [
                el.getAttribute("aria-label"),
                el.getAttribute("title"),
                el.textContent,
            ].filter(Boolean).join(" ").trim();

            document
                .querySelectorAll("[" + createMark + "='1']")
                .forEach((el) => el.removeAttribute(createMark));

            const words = /new post|post|gönderi|gonderi/i;
            const roots = [
                ...document.querySelectorAll("[role='dialog'],[role='menu']"),
                document.body,
            ].filter(isVisible);

            const candidates = roots.flatMap((root) => Array
                .from(root.querySelectorAll("a,button,[role='button'],[role='menuitem'],[tabindex='0']"))
                .filter((el) => {
                    if (!isVisible(el)) return false;
                    if (el.matches(":disabled") ||
                        el.getAttribute("aria-disabled") === "true") {
                        return false;
                    }
                    const label = labelOf(el);
                    if (/create|oluştur|olustur/i.test(label)) return false;
                    return words.test(label);
                })
                .map((el) => {
                    const label = labelOf(el);
                    let score = 0;
                    if (/post|gönderi|gonderi|new post/i.test(label)) score += 10000;
                    return { el, score, label };
                }));

            candidates.sort((a, b) => b.score - a.score);
            if (!candidates.length) return false;
            candidates[0].el.setAttribute(createMark, "1");
            candidates[0].el.setAttribute(
                "data-ytdl-instagram-create-label",
                candidates[0].label || "create menu"
            );
            return true;
        }
        """,
        arg={"createMark": CREATE_MARK},
        timeout=10000,
    )

    item = page.locator(f"[{CREATE_MARK}='1']").first
    label = item.get_attribute("data-ytdl-instagram-create-label") or "create menu"
    item.click(timeout=10000)
    return label


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
        _click_create_button(page)
        try:
            _mark_upload_file_input(page, timeout=2000)
            label = "Create"
        except Exception:
            _click_create_menu_item(page)
            label = "Create > Post"
            _mark_upload_file_input(page, timeout=30000)
        emit({"stage": "create_opened", "selector": label})
        return
    except Exception as e:
        raise RuntimeError(
            "Instagram Reels create flow could not be opened: "
            + str(e)
        )


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

                const labelOf = (el) => {
                    const nearby = el.closest("label,[role='textbox']") || el.parentElement;
                    return [
                        el.getAttribute("aria-label"),
                        el.getAttribute("placeholder"),
                        el.getAttribute("data-placeholder"),
                        el.getAttribute("title"),
                        nearby ? nearby.textContent : "",
                    ].filter(Boolean).join(" ").replace(/\\s+/g, " ").trim();
                };

                const badWords = /search|comment|emoji|location|tag people|alt text/i;
                const captionWords =
                    /caption|write a caption|description|açıklama|aciklama|başlık|baslik/i;

                const nodes = [
                    ...root.querySelectorAll("textarea"),
                    ...root.querySelectorAll("[contenteditable='true'][role='textbox']"),
                    ...root.querySelectorAll("[contenteditable='true']"),
                ];

                const candidates = [];
                for (const el of nodes) {
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
                    const label = labelOf(el);
                    if (badWords.test(label)) continue;

                    let score = 0;
                    if (captionWords.test(label)) score += 10000;
                    if (el.matches("textarea")) score += 1000;
                    if (el.getAttribute("role") === "textbox") score += 750;
                    if (el.isContentEditable) score += 500;
                    score += Math.min(rect.width, 600);
                    candidates.push({ el, score, label });
                }

                candidates.sort((a, b) => b.score - a.score);
                if (!candidates.length) return false;
                candidates[0].el.setAttribute(captionMark, "1");
                candidates[0].el.setAttribute(
                    "data-ytdl-instagram-caption-label",
                    candidates[0].label || "caption"
                );
                return true;
            }
            """,
            arg={"captionMark": CAPTION_MARK},
            timeout=5000,
        )
        return True
    except Exception:
        return False


def _normalize_caption_value(text: str) -> str:
    return (text or "").replace("\r\n", "\n").replace("\r", "\n").replace("\u200b", "")


def _read_caption_editor(editor) -> str:
    value = editor.evaluate(
        """
        (el) => {
            if ("value" in el) return el.value || "";
            return el.innerText || el.textContent || "";
        }
        """
    )
    return _normalize_caption_value(value or "")


def _compact_caption_probe(text: str) -> str:
    compact = "".join(_normalize_caption_value(text).split()).lower()
    return compact[:80]


def _caption_was_written(written: str, expected: str) -> bool:
    written = _normalize_caption_value(written).strip()
    if not written:
        return False
    expected_probe = _compact_caption_probe(expected)
    if not expected_probe:
        return True
    written_compact = _compact_caption_probe(written)
    return expected_probe[:20] in written_compact


def _fill_caption(page, caption: str) -> None:
    if not _mark_caption_editor(page):
        raise RuntimeError("Instagram caption field was not found")

    caption = _normalize_caption_value(caption).strip()
    if not caption:
        raise RuntimeError("Caption is empty; refusing to publish without a caption")
    if len(caption) > INSTAGRAM_CAPTION_LIMIT:
        caption = caption[:INSTAGRAM_CAPTION_LIMIT].rstrip()
        emit({
            "stage": "caption_truncated",
            "selector": f"{INSTAGRAM_CAPTION_LIMIT} chars",
        })

    editor = page.locator(f"[{CAPTION_MARK}='1']").first
    editor.fill(caption, timeout=15000)
    page.wait_for_timeout(700)
    written = _read_caption_editor(editor)
    if _caption_was_written(written, caption):
        emit({"stage": "caption_filled", "selector": f"{len(caption)} chars"})
        return

    editor.click(timeout=10000)
    page.keyboard.press("Control+A")
    page.keyboard.press("Delete")
    page.keyboard.insert_text(caption)
    page.wait_for_timeout(500)
    written = _read_caption_editor(editor).strip()
    if not _caption_was_written(written, caption):
        if not written:
            raise RuntimeError("Caption stayed empty after filling; publish was stopped")
        raise RuntimeError("Caption verification failed; publish was stopped")
    emit({"stage": "caption_filled", "selector": f"{len(caption)} chars"})


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
        arg={"actionMark": ACTION_MARK, "kindMark": ACTION_KIND_MARK, "kind": kind},
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
        raise RuntimeError("Instagram caption step could not be reached")


def _wait_for_post_success(page) -> bool:
    try:
        page.wait_for_function(
            """
            ({ doneMark }) => {
                const visible = (el) => {
                    if (!(el instanceof HTMLElement)) return false;
                    const rect = el.getBoundingClientRect();
                    const style = window.getComputedStyle(el);
                    return rect.width > 0 && rect.height > 0 &&
                        style.display !== "none" &&
                        style.visibility !== "hidden" &&
                        style.pointerEvents !== "none";
                };

                const enabled = (el) => {
                    return !el.matches(":disabled") &&
                        el.getAttribute("aria-disabled") !== "true" &&
                        el.closest("[aria-disabled='true']") === null;
                };

                document
                    .querySelectorAll("[" + doneMark + "='1']")
                    .forEach((el) => el.removeAttribute(doneMark));

                const dialog = Array
                    .from(document.querySelectorAll("[role='dialog']"))
                    .filter(visible)
                    .at(-1);
                if (!dialog) return true;

                const successWords =
                    /shared|posted|uploaded|paylaşıldı|paylasildi|yayınlandı|yayinlandi/i;
                const dialogText = (dialog.textContent || "").trim();
                const buttons = Array
                    .from(dialog.querySelectorAll("button,[role='button'],[tabindex='0']"))
                    .filter((el) => visible(el) && enabled(el));

                const exactDone = buttons.find((el) => {
                    const text = (el.textContent || "").trim();
                    const label = [
                        el.getAttribute("aria-label"),
                        el.getAttribute("title"),
                        text,
                    ].filter(Boolean).join(" ");
                    const compact = label.toLocaleLowerCase("tr-TR")
                        .replace(/\\s+/g, "");
                    return text.length <= 30 && (
                        /^(done)+$/.test(compact) ||
                        /^(bitti)+$/.test(compact) ||
                        /^(tamam)+$/.test(compact) ||
                        /^ok$/.test(compact)
                    );
                });
                if (exactDone) {
                    exactDone.setAttribute(doneMark, "1");
                    return true;
                }

                if (successWords.test(dialogText) && buttons.length === 1) {
                    buttons[0].setAttribute(doneMark, "1");
                    return true;
                }

                return false;
            }
            """,
            arg={"doneMark": DONE_MARK},
            timeout=180000,
        )
        done = page.locator(f"[{DONE_MARK}='1']").first
        if done.count() > 0:
            label = done.evaluate("(el) => (el.textContent || '').trim()") or "Done"
            done.scroll_into_view_if_needed(timeout=10000)
            done.click(timeout=10000)
            emit({"stage": "done_clicked", "selector": label})
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
                    return !Array
                        .from(document.querySelectorAll("[role='dialog']"))
                        .some(visible);
                }
                """,
                timeout=30000,
            )
        return True
    except Exception:
        return False


def cmd_upload(args) -> int:
    file_path = Path(args.file).resolve()
    if not file_path.is_file():
        emit({"ok": False, "error": f"File not found: {file_path}"})
        return 1

    with sync_playwright() as p:
        ctx = open_context(p, headless=args.headless)
        try:
            page = ctx.new_page()
            emit({"stage": "open"})
            _open_create_flow(page)

            emit({"stage": "uploading"})
            _upload_file_input(page).set_input_files(str(file_path))
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
                "error": "Instagram success confirmation was not detected within 3 minutes",
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
