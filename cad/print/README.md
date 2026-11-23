# Print files

`keycap.stl` is one keycap for a Cherry MX switch, top face down as it prints. Print four: three grey and one orange for the menu key. No supports.

Print `../test-fits/15-keycap-stem-fit.stl` first. It has three stem sockets, marked with one, two and three notches (tight, as modelled, loose).
Push each onto a switch and say which one holds firmly without forcing; the keycap uses the two-notch size.

Regenerate both with `../fusion/case/keycap_export.py` (run after `case_mk3.py` in the same script). The case parts themselves are not exported
here yet; `../fusion/case/print_export.py` makes them once the test fits are right.
