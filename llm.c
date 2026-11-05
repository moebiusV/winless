/* llm.c -- what the model is told, and how it is run.  Copyright (C) 2026 David Walther.  GPL-3.0-or-later.
 *
 * Pure functions only (no processes, no network): the instructions, the request, the command line of each backend.  The server runs
 * the command; edits.c checks what comes back.
 */
#define _POSIX_C_SOURCE 200809L
#include "winless.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char SYSTEM_PROMPT[] =
    "You are an editing assistant for an author who is revising their own prose. The author has marked up a document with annotations. "
    "Propose edits that address the annotations, and nothing else.\n"
    "\n"
    "Annotation kinds:\n"
    "- note: an instruction from the author about the marked text. Do what it says.\n"
    "- slop: the marked text is the kind of writing the author does not want (filler, cliche, vagueness, padding, over-explaining, "
    "empty intensifiers). Fix the marked text. You may also fix other places in the document with clearly the same problem; give those "
    "edits \"also\": true and still cite the slop annotation's id.\n"
    "- keep: the marked text is good. Do not change it in any way, and do not include any of it in the text of an edit.\n"
    "\n"
    "Rules:\n"
    "- Leave everything that no annotation covers exactly as it is. Do not fix unrelated things. Keep the author's voice, facts, numbers, "
    "names, citations, and markup (markdown, headings, tokens such as {{c:id00018}}) unless an annotation says otherwise. Do not add facts.\n"
    "- If an annotation needs no change, or you cannot address it without guessing, leave it out. Never invent a change to look busy.\n"
    "- Answer with JSON only, no prose and no code fence, in exactly this form:\n"
    "  {\"edits\":[{\"annotations\":[<id>],\"find\":\"<text copied exactly from the document>\",\"replace\":\"<the new text>\","
    "\"why\":\"<a few words>\",\"also\":false}]}\n"
    "  List in \"annotations\" every annotation the edit answers.\n"
    "- \"find\" must be copied exactly from the document (same characters, spaces, punctuation and line breaks) and must occur exactly "
    "once in the document. Use the shortest span that is unique and contains everything you change; add neighbouring words until it is unique.\n"
    "- Edits must not overlap or touch one another: each \"find\" must end before the next one begins. When two annotations affect the same "
    "or adjacent words, answer them with ONE edit that covers both, list both ids in \"annotations\", and say so in \"why\".\n"
    "- To delete text, use an empty \"replace\".\n"
    "- If there is nothing to change, answer {\"edits\":[]}.\n";

const char *llm_system_prompt(void) { return SYSTEM_PROMPT; }

/* The user message: the whole document (for context) and the annotations, as JSON the model cannot misread. */
void llm_user_message(sb_t *out, const char *doc, size_t doclen, const char *annotations_json) {
    sb_puts(out, "<document>\n");
    sb_put(out, doc, doclen);
    if (doclen == 0 || doc[doclen - 1] != '\n') sb_putc(out, '\n');
    sb_puts(out, "</document>\n\n<annotations>\n");
    sb_puts(out, annotations_json);
    sb_puts(out, "\n</annotations>\n\nPropose the edits as JSON.\n");
}

/* The request as JSON, for a command given in $WINLESS_LLM: {"system": ..., "user": ..., "model": ...} on its standard input. */
void llm_request_json(sb_t *out, const char *system, const char *user, size_t userlen, const char *model) {
    sb_puts(out, "{\"system\":"); json_put_string(out, system, strlen(system));
    sb_puts(out, ",\"user\":"); json_put_string(out, user, userlen);
    sb_puts(out, ",\"model\":"); if (model && *model) json_put_string(out, model, strlen(model)); else sb_puts(out, "null");
    sb_putc(out, '}');
}

/* The `claude -p` command: no tools, no session kept, our own system prompt; the user message goes on standard input and the reply
 * comes back on standard output.  The executable is $WINLESS_CLAUDE if set (a test double, or a path), else `claude`.
 * Returns a NULL-terminated argv (free with llm_free_argv). */
char **llm_claude_argv(const char *system, const char *model, const char *effort) {
    const char *exe = getenv("WINLESS_CLAUDE"); if (!exe || !*exe) exe = "claude";
    char **a = xmalloc(16 * sizeof *a); int n = 0;
    a[n++] = xstrdup(exe); a[n++] = xstrdup("-p");
    a[n++] = xstrdup("--tools"); a[n++] = xstrdup("");
    a[n++] = xstrdup("--no-session-persistence");
    a[n++] = xstrdup("--system-prompt"); a[n++] = xstrdup(system);
    if (model && *model) { a[n++] = xstrdup("--model"); a[n++] = xstrdup(model); }
    if (effort && *effort) { a[n++] = xstrdup("--effort"); a[n++] = xstrdup(effort); }
    a[n] = NULL;
    return a;
}
void llm_free_argv(char **a) { if (!a) return; for (char **p = a; *p; p++) free(*p); free(a); }
