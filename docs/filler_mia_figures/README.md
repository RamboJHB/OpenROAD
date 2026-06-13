# Filler Insertion & Min-Implant-Area (MIA) — Figure Set

Teaching figures used in `../filler_insertion_explained.md`,
`../filler_insertion_study_report.md`, and `../filler_insertion_summary.md`
to explain why multi-Vt filler insertion is constrained by **Min Implant
Area / Width (MIA)** rules, and how those constraints behave **within a
row**, **across rows (inter-row)**, and for **vertically split (top/bottom)
Vt cells**.

> Schematic teaching figures, not tapeout layouts. Dimensions (`Wmin = 4`,
> widths `2`/`4`) are illustrative units chosen to make the rule arithmetic
> obvious.

## Figures

| File | Topic |
|------|-------|
| `fig1_concept.png` | What MIA is: a Vt-implant region narrower/smaller than the minimum is a violation. |
| `fig2_case1_one_filler.png` | **Case 1** — one same-Vt filler extends a too-narrow implant band to `≥ Wmin`. |
| `fig3_case2_two_same.png` | **Case 2** — two same-Vt fillers needed to bridge a gap and reach `Wmin`. |
| `fig5_interrow.png` | **Inter-row MIA** — adjacent mirrored rows share a power rail, so a 2-row-tall narrow implant violates; filling only one row *creates* an inter-row violation; the fix is to fill **both rows aligned**, also satisfying implant **spacing**. |
| `fig6_case3_vertical.png` | **Vertically split Vt cell** — a cell with **top = LVT (PMOS) / bottom = HVT (NMOS)**. Implants are really two horizontal bands; MIA is evaluated per band; a single matching top-LVT/bottom-HVT filler fixes both bands. |
| `fig7_spacing.png` | **Implant spacing (`Smin`)** — co-constraint with MIA. (A) the min-spacing rule; (B) filling a gap must **abut OR leave ≥ Smin**, never a sub-Smin sliver; (C) a too-narrow filler can break **MIA and spacing at once**. |

## Note on the implant geometry (why `fig6` is the "correct Case 3")

An earlier draft drew a mixed-Vt cell with the two Vt halves split
**left/right**, suggesting "one cell needs two different fillers." That split
is **not** how standard cells work and was removed. In reality PMOS sits at
the top (N-well, near VDD) and NMOS at the bottom (near VSS), so the
Vt-adjust implants form **top/bottom bands**, not left/right halves —
`fig6` shows this corrected picture. The genuine "two different fillers"
situation re-emerges across **rows** (inter-row): the shared-VDD PMOS band
spans two rows, so completing it can require one filler in the adjacent row
plus another for the NMOS band in the current row. See
`../filler_insertion_study_report.md` §7 and `../filler_insertion_summary.md`.

## Regenerating

The three scripts under `scripts/` produce the PNGs (require `matplotlib`).
By default they write into `/tmp/mia_figs/`; copy the outputs back here, or
edit the `OUT` path at the top of each script.

| Script | Produces |
|--------|----------|
| `scripts/make_mia_figs.py` | `fig1`–`fig3` |
| `scripts/make_interrow_fig.py` | `fig5` |
| `scripts/make_vsplit_fig.py` | `fig6` |
| `scripts/make_spacing_fig.py` | `fig7` |

```bash
python3 scripts/make_mia_figs.py
python3 scripts/make_interrow_fig.py
python3 scripts/make_vsplit_fig.py
python3 scripts/make_spacing_fig.py
```
