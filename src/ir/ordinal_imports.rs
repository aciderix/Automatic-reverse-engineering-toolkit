//! Resolution of PE imports **by ordinal** to their export name.
//!
//! Some system DLLs export functions by ordinal only (no name in the importer's
//! table): the classic case is `COMCTL32.dll`, whose Win95-era ABI imports
//! `InitCommonControls` as ordinal 17. The loader sees such a thunk as a raw
//! `0x80000000 | ordinal` value; without a name it cannot route the `call` to a
//! shim, so the indirect call aborts on the opaque address. Mapping `(dll, ordinal)`
//! to the export name lets the normal name-based shim resolution take over (an
//! unimplemented one then aborts *by name*, a strictly clearer sound failure).
//!
//! The tables are ground-truth ABI data. `COMCTL32` is extracted verbatim from the
//! export table of the very `comctl32.dll` that Wine — our correctness oracle — runs;
//! Wine matches Microsoft's ordinal numbering by design (else ordinal-importing apps
//! would break under Wine), so this mapping is correct *by construction* relative to
//! the oracle. Ordinals are ABI-stable across comctl32 versions for the classic range.

/// COMCTL32.dll ordinal -> export name (ordinal base 2), sorted by ordinal.
static COMCTL32: &[(u16, &str)] = &[
    (2, "MenuHelp"),
    (3, "ShowHideMenuCtl"),
    (4, "GetEffectiveClientRect"),
    (5, "DrawStatusTextA"),
    (6, "CreateStatusWindowA"),
    (7, "CreateToolbar"),
    (8, "CreateMappedBitmap"),
    (12, "CreatePropertySheetPage"),
    (13, "MakeDragList"),
    (14, "LBItemFromPt"),
    (15, "DrawInsert"),
    (16, "CreateUpDownControl"),
    (17, "InitCommonControls"),
    (18, "CreatePropertySheetPageA"),
    (19, "CreatePropertySheetPageW"),
    (20, "CreateStatusWindow"),
    (21, "CreateStatusWindowW"),
    (22, "CreateToolbarEx"),
    (23, "DestroyPropertySheetPage"),
    (24, "DllGetVersion"),
    (25, "DllInstall"),
    (26, "DPA_GetSize"),
    (27, "DrawShadowText"),
    (28, "DrawStatusText"),
    (29, "DrawStatusTextW"),
    (30, "DSA_Clone"),
    (31, "DSA_GetSize"),
    (32, "FlatSB_EnableScrollBar"),
    (33, "FlatSB_GetScrollInfo"),
    (34, "FlatSB_GetScrollPos"),
    (35, "FlatSB_GetScrollProp"),
    (36, "FlatSB_GetScrollRange"),
    (37, "FlatSB_SetScrollInfo"),
    (38, "FlatSB_SetScrollPos"),
    (39, "FlatSB_SetScrollProp"),
    (40, "FlatSB_SetScrollRange"),
    (41, "FlatSB_ShowScrollBar"),
    (42, "GetMUILanguage"),
    (43, "HIMAGELIST_QueryInterface"),
    (44, "ImageList_Add"),
    (45, "ImageList_AddIcon"),
    (46, "ImageList_AddMasked"),
    (47, "ImageList_BeginDrag"),
    (48, "ImageList_CoCreateInstance"),
    (49, "ImageList_Copy"),
    (50, "ImageList_Create"),
    (51, "ImageList_Destroy"),
    (52, "ImageList_DragEnter"),
    (53, "ImageList_DragLeave"),
    (54, "ImageList_DragMove"),
    (55, "ImageList_DragShowNolock"),
    (56, "ImageList_Draw"),
    (57, "ImageList_DrawEx"),
    (58, "ImageList_DrawIndirect"),
    (59, "ImageList_Duplicate"),
    (60, "ImageList_EndDrag"),
    (61, "ImageList_GetBkColor"),
    (62, "ImageList_GetDragImage"),
    (63, "ImageList_GetFlags"),
    (64, "ImageList_GetIcon"),
    (65, "ImageList_GetIconSize"),
    (66, "ImageList_GetImageCount"),
    (67, "ImageList_GetImageInfo"),
    (68, "ImageList_GetImageRect"),
    (69, "ImageList_LoadImage"),
    (70, "ImageList_LoadImageA"),
    (75, "ImageList_LoadImageW"),
    (76, "ImageList_Merge"),
    (77, "ImageList_Read"),
    (78, "ImageList_Remove"),
    (79, "ImageList_Replace"),
    (80, "ImageList_ReplaceIcon"),
    (81, "ImageList_SetBkColor"),
    (82, "ImageList_SetDragCursorImage"),
    (83, "ImageList_SetFilter"),
    (84, "ImageList_SetFlags"),
    (85, "ImageList_SetIconSize"),
    (86, "ImageList_SetImageCount"),
    (87, "ImageList_SetOverlayImage"),
    (88, "ImageList_Write"),
    (89, "ImageList_WriteEx"),
    (90, "InitCommonControlsEx"),
    (91, "InitMUILanguage"),
    (92, "InitializeFlatSB"),
    (93, "PropertySheet"),
    (94, "PropertySheetA"),
    (95, "PropertySheetW"),
    (96, "RegisterClassNameW"),
    (97, "UninitializeFlatSB"),
    (98, "_TrackMouseEvent"),
    (320, "DSA_Create"),
    (321, "DSA_Destroy"),
    (322, "DSA_GetItem"),
    (323, "DSA_GetItemPtr"),
    (324, "DSA_InsertItem"),
    (325, "DSA_SetItem"),
    (326, "DSA_DeleteItem"),
    (327, "DSA_DeleteAllItems"),
    (328, "DPA_Create"),
    (329, "DPA_Destroy"),
    (330, "DPA_Grow"),
    (331, "DPA_Clone"),
    (332, "DPA_GetPtr"),
    (333, "DPA_GetPtrIndex"),
    (334, "DPA_InsertPtr"),
    (335, "DPA_SetPtr"),
    (336, "DPA_DeletePtr"),
    (337, "DPA_DeleteAllPtrs"),
    (338, "DPA_Sort"),
    (339, "DPA_Search"),
    (340, "DPA_CreateEx"),
    (344, "TaskDialog"),
    (345, "TaskDialogIndirect"),
    (380, "LoadIconMetric"),
    (381, "LoadIconWithScaleDown"),
    (385, "DPA_EnumCallback"),
    (386, "DPA_DestroyCallback"),
    (387, "DSA_EnumCallback"),
    (388, "DSA_DestroyCallback"),
    (400, "CreateMRUListW"),
    (401, "AddMRUStringW"),
    (403, "EnumMRUListW"),
    (410, "SetWindowSubclass"),
    (411, "GetWindowSubclass"),
    (412, "RemoveWindowSubclass"),
    (413, "DefSubclassProc"),
];

/// Look up the export name for an import by ordinal from a known system DLL.
/// `dll` is matched case-insensitively (import descriptors vary: `COMCTL32.dll`,
/// `comctl32.dll`). Returns `None` for an unknown DLL or ordinal — the caller then
/// leaves the slot unresolved (a sound abort on use), never guesses a name.
pub fn ordinal_import_name(dll: &str, ordinal: u16) -> Option<&'static str> {
    let base = dll.strip_suffix(".dll").or_else(|| dll.strip_suffix(".DLL")).unwrap_or(dll);
    let table: &[(u16, &str)] = if base.eq_ignore_ascii_case("comctl32") {
        COMCTL32
    } else {
        return None;
    };
    table.binary_search_by_key(&ordinal, |&(o, _)| o).ok().map(|i| table[i].1)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn comctl32_init_common_controls_is_ordinal_17() {
        assert_eq!(ordinal_import_name("COMCTL32.dll", 17), Some("InitCommonControls"));
        assert_eq!(ordinal_import_name("comctl32.dll", 17), Some("InitCommonControls"));
    }
    #[test]
    fn unknown_dll_or_ordinal_is_none() {
        assert_eq!(ordinal_import_name("kernel32.dll", 17), None);
        assert_eq!(ordinal_import_name("COMCTL32.dll", 9999), None);
    }
    #[test]
    fn comctl32_table_is_sorted_by_ordinal() {
        assert!(COMCTL32.windows(2).all(|w| w[0].0 < w[1].0), "COMCTL32 must be sorted, unique");
    }
}
