# Case Model (3d-model)

The made case is not hand-modeled: it is generated entirely by the parametric `3d-model/make_case.py`, where every part has an object name and variables in the script. **Before changing the case, consult the parts dictionary: [Case parts dictionary](../../3d-model/PARTS.md)** (front frame front, back shell back, plunger named by MIC-end / USB-end orientation).

Two indexes:

- **`3d-model/PARTS.md`** — part → script object/variable mapping (text version).
- **`3d-model/PARTS-diagram.svg`** — annotated parts diagram (back shell top view, with part names + dimensions + variable names drawn on the image).
  **Trigger phrase: "3d 外壳标注图".** When the user says this, open that SVG directly — never substitute a textual description.

Rules:

- In change proposals, comments and branch names, always use the dictionary's part names — never "the left one / the top one / that middle block".
- Orientation follows the dictionary's coordinate system: `x=0` is the **MIC end**, `x=OUT_L` is the **USB end**; for the front frame `z=0` is the outer panel, for the back shell `z=0` is the outer back.
- Changes land on variables in `make_case.py`; after editing, re-run the script to export the three STLs (front frame, back shell, plunger) and re-render `preview.png`. Never edit STL / 3MF artifacts directly.
- `boss_top`, `seat_z` and the plunger length form a linked chain: changing the button-post height requires changing the plunger too — see the dictionary for details.
- When a part's position or size changes, update `PARTS-diagram.svg` in sync (the diagram is hand-drawn from script parameters; if not synced it drifts).
