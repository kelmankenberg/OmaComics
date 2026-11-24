![OmaComics reader](share/omacomics-reader.png)

# OmaComics

OmaComics is a keyboard-friendly comic library and reader for Omarchy. It scans selected folders in place, reads `ComicInfo.xml` metadata when present, and never modifies the original comics.

The library supports CBZ, CBR, CB7, CBT, and PDF files. Search, collections, recently read, bookmarks, page progress, and adjustable cover thumbnails are included. The reader supports single pages, two-page spreads, right-to-left manga order, fit modes, zoom, and panning.

On startup, OmaComics checks library folders for new or changed files instead of re-indexing every book. Existing libraries keep their saved metadata; archive page indexes are built as needed for covers and reading.

The companion `kel.omacomics` bar widget shows the current title and progress. Click it for resume/open actions; use Omarchy's normal bar controls to move it. Right-click opens the library directly.

## Requirements

- Qt 6, CMake, and a C++17 compiler
- `7zip` for CBZ/CBR/CB7/CBT archives
- Poppler (`pdfinfo` and `pdftoppm`) for PDF indexing and rendering
- An XDG desktop portal for choosing library folders

## Build and install locally

```sh
bin/test
cmake --install build --prefix ~/.local
```

After installing the app, put the widget on the right side of the bar:

```sh
omarchy bar put kel.omacomics --section right
```

Choose **Add folder** in OmaComics to select a library. The app persists its index and reading progress under `~/.local/share/omacomics/`.

Type a word or phrase and press Enter to add a search chip. All chips must match; remove any chip with its × button.

Use the **List view** and **Folder tree** buttons beside **Library folders** to switch between the existing cover list and an expandable directory tree. The same compact button group includes Comic Vine scraping and the ⋮ selection menu. The view choice is remembered. Both views use the same search, All books/Recently read, and collection filters. Check comics to select several, or check a folder to select every matching comic in it and its subfolders, even when collapsed. Unchecking a folder clears those comics; partially selected folders show a mixed checkbox. The ⋮ menu can select all visible comics or clear the selection. Changing filters removes hidden comics from the selection.

Select **Scrape…** to look up selected comics using the [Comic Vine API](https://comicvine.gamespot.com/api/). Enter your own API key locally; it is masked and excluded from application files and library backups. With **Remember API key securely** enabled (the default), `secret-tool` saves it in the desktop Secret Service credential store and retrieves it on later launches. The helper is optional: without it, or if the store is unavailable or access is cancelled, the app explicitly reports that storage is unavailable or failed and never falls back to a plain-text file. Uncheck the option for session-only keys, or choose **Forget API key** to remove a saved key. No metadata is fetched at startup or while indexing. **Search** sends only the editable query and API key to Comic Vine over HTTPS, never comic files or their full paths. Choose a result, review the missing fields, then **Apply and next**, or **Skip**. Filename-derived titles can be filled; other existing metadata and reading progress are preserved. Scraped metadata stays in the local library index across restarts and rescans, with a safety backup before application. Original archives are never changed.

Comic Vine supplies titles, series, issue numbers, publication years, writers, publishers, and summaries when available; covers still come from the local comic. The API is for personal, non-commercial use under [Comic Vine's terms](https://comicvine.gamespot.com/api/). Lookups are cached for the session and spaced at least 20 seconds apart per API resource to stay below its documented 200 requests/resource/hour limit. Rate-limit and connection errors are shown in the review dialog; closing it cancels pending requests. No matches are applied automatically.

Open **Settings** using the gear beside the title or `Ctrl+,`. Settings temporarily replaces the left library panel; the back arrow or Escape returns to the library without changing the open comic or filters.

The category chooser provides **Startup**, **Reading**, **Library**, **Backups**, **Image cache**, **Comic Vine**, and **Feedback**. Preferences are saved atomically in `~/.local/share/omacomics/settings.json`, separately from the index, and survive restarts and index restoration.

**Startup** can open the library (default), preview the last comic without opening the reader, or resume reading automatically. A file passed on the command line or `--resume` takes precedence. **Reading** remembers fit mode, zoom, and single/two-page layout. Arrow keys, wheel navigation, and mouse buttons are independent persistent preferences, all enabled by default; the reader's navigation menu offers the same controls.

Manga direction is **per comic**, not a global preference. Change the current comic with the reader's Manga switch. To apply it to several comics, check comics or whole folders in the library, then choose **Manga direction for selected** or **Left-to-right for selected** in its selection menu. Folder selection includes matching descendants, including collapsed subfolders. The direction survives rescans and restarts.

**Library** remembers list/tree view, thumbnail size (including no thumbnails), and sidebar visibility. **Check library at startup** defaults on; disable it for manual-only library scans. Scans check file stamps and index only new or changed files, asynchronously. You can open indexed comics and continue turning pages while other books are being indexed; a requested unindexed book is prioritized.

Under **Backups**, choose the folder for automatic daily backups, safety backups, and **Back up now**, or restore an existing index. The default is `~/.local/share/omacomics/backups/`. Changing the location leaves existing backups where they are. This machine-local choice is saved in `~/.local/share/omacomics/settings.json`, separately from the library, and is not changed by restoring an index. An unavailable backup folder is reported rather than silently switching locations.

Automatic retention defaults to **30 days**; choose 7, 30, or 90 days, or **Keep forever**. After a successful automatic snapshot, cleanup runs in the background in the current backup folder. It removes recognized expired `daily-*.json` and new `safety-*.json` snapshots, retaining the selected number of calendar days including today. **Manual backups are never pruned.** Older `omacomics-*.json` files are also kept because previous versions used that prefix for both manual and safety backups. Unknown, malformed, and symlinked files are not removed. Changing to a shorter retention period can remove expired automatic snapshots immediately.

Restoring replaces the current library and reading history after confirmation. OmaComics saves one daily backup and a safety backup before removing a folder, removing a recent-history entry, applying scraped metadata, or restoring an index. Backups contain the index and reading progress, not comic files. Removing a folder never deletes the files.

**Image cache** measures generated covers and page images and can clear them after confirmation, once the reader is closed and background extraction has finished. Page-list indexes, original comics, metadata, bookmarks, and reading progress are retained, so clearing images does not force a full library reindex. Visible covers may be generated again immediately.

**Comic Vine** contains API-key configuration and a persistent choice of series/issue-number searches (default) or title searches. Lookups always remain manually initiated, reviewed, and fill-missing-only.

Under **Feedback**, **Report a bug** and **Request a feature** open the corresponding GitHub issue forms in your browser. Review and submit them yourself with a GitHub account; no diagnostics, library paths, or files are automatically attached. The forms live in `.github/ISSUE_TEMPLATE/` and become available when published on the repository's default branch.

In the reader, the ⋮ menu independently enables arrow keys, the mouse wheel, and mouse buttons for page navigation. Right-click advances and left-click goes back. The wheel scrolls an enlarged page before turning at its edge; drag or use the scrollbars to pan.

## Keyboard

| Key | Action |
| --- | --- |
| `Return` | Read the selected comic |
| `Left` / `Right` | Turn pages |
| `Space` / `Backspace` | Next / previous page |
| `B` | Toggle a bookmark |
| `Ctrl+B` | Show or hide the library panel |
| `Ctrl+,` | Open or close Settings |
| `Escape` | Return from Settings, or close the reader |
| `?` | Show shortcuts |
| `Q` | Quit |
