# Backport tracking: Current (non-tar) version

Fixes implemented in the Tar version's session that also apply to the
current, simpler, stable version (the one from before tabs/tar
management). Not yet applied there. Revisit this list if/when work
resumes on that version specifically.

## 1. Detail view could open against the wrong preview (both modes)

**Bug:** Moving the grid cursor and immediately pressing A (or
tapping), before the background loader finished updating the top
screen, opened the More screen for the newly-selected item while the
top screen still showed the *previous* selection's image. Real risk:
a wrong-screenshot delete looked plausible under that mismatch.

**Fix implemented (Tar version):** a `current_preview_ready()` check
gates every path into `enter_detail_view()` -- both the `KEY_A` press
and the tap-to-open-directly path -- on
`s_visibleIndices[s_selected] == s_previewLoadedPairIndex`, i.e. the
preview must have actually *finished* loading for the current
selection, not merely been requested. Tapping an unselected cell now
just selects it (consistent with a fresh cursor move); a second tap or
A opens it once the preview has caught up.

Applies identically to 3DS and DS content -- no mode-specific logic
involved, so should port over directly.

## 2. Button-press SFX played even when the press did nothing

**Bug:** `SFX_BUTTON` fired on every `A/B/X/Y/L/R/Select` press
unconditionally, including e.g. B on the plain Album screen, which has
no Back target there and does nothing.

**Fix implemented (Tar version):** moved the sound from
"before the input switch, unconditional" to "after the input switch,
only if a snapshot of meaningful state actually changed" -- state,
selection, batch mode, filter fields, detail/confirm selection, DS
tab, and a checksum of the batch-selection array, compared before vs.
after processing input for the frame. Not exhaustive: a couple of
toggle-style actions that don't touch any tracked field (the DS
widescreen flag) needed an explicit `audio_play(SFX_BUTTON)` call of
their own at their toggle site instead of relying on the diff.

Porting note: the current version doesn't have `s_dsTab` (no tabs) --
drop that field from the snapshot, keep the rest.

## 3. DS-mode entry: grid loads visibly behind the extract-prompt popup

**Not yet designed, current-version-only** (the Tar version replaced
the automatic on-entry prompt with an explicit Start-triggered Extract
All, so this specific interaction doesn't exist there anymore).

**Problem:** in the current version, entering DS mode with unextracted
screenshots in the tar shows the "screenshots.tar found -- extract?"
popup *while* the grid underneath is still scanning/loading
thumbnails in the background, causing visible lag and dropped input on
the popup itself.

**Proposed fix:** defer the grid's data load (the scan + thumbnail
kickoff) until *after* the user answers the prompt, rather than
starting it immediately on entry and layering the popup on top.
Needs actual design work when this version is revisited -- not
implemented anywhere yet.
