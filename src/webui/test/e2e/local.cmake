# The page scripts of BitAssets' own pages, run against a live mainchain as webui_e2e_<name>
# (webui_local_e2e of test/CMakeLists.txt, from the template).
# bitassets.js: an asset issued, minted, pooled, swapped, auctioned and sent.
webui_local_e2e(bitassets 29770)
# bitassets_simple.js: Simple mode, as a user chooses it: Create a token, Swap from its plain quote, My assets.
webui_local_e2e(bitassets_simple 41760 --js-var SIMPLE=true)
# bitassets_logo.js: an asset registered with a logo (picker, confirmation, lists, Home, Overview) and sent (Transactions).
webui_local_e2e(bitassets_logo 41790)
# ba_bulk.js: 300 assets made by a script while the Transactions page shows (user feedback 2026-10-19, item 10): at most
# 10 frames over 50 ms, the UI thread of the host answering within a second (PERF lines: the frames, the calls by method).
webui_local_e2e(ba_bulk 41820 --js-var COUNT=300 --js-var "PAGE=\"transactions\"" --js-var MAXLONG=10)
# ba_ignore.js: a spam asset received, ignored from its notice, gone from Home, My assets and the Transactions page, back once unignored.
webui_local_e2e(ba_ignore 41850)
