# Test data

Real MIDI files the loader tests read. Both are public domain.

| File | Piece | Source | License |
| --- | --- | --- | --- |
| `bach-bwv846-prelude.mid` | J. S. Bach, Prelude in C major, BWV 846 | Mutopia Project, `https://www.mutopiaproject.org/ftp/BachJS/BWV846/wtk1-prelude1/wtk1-prelude1.mid` | Public Domain (`copyright = "Public Domain"` in the LilyPond source) |
| `satie-gymnopedie-1.mid` | Erik Satie, Gymnopédie No. 1 | Mutopia Project, `https://www.mutopiaproject.org/ftp/SatieE/gymnopedie_1/gymnopedie_1.mid` | Public Domain (`license = "Public Domain"` in the LilyPond source) |

Downloaded 2026-10-12. LilyPond wrote both, with one track per staff named after its staff.

## Score files (MusicXML, MuseScore)

One real piece, in the three score formats the loaders read. Source: the OpenScore Lieder corpus
(`https://github.com/OpenScore/Lieder`, commit `38c5db510224d9facdc4b08d741fc788cfb58ea8`, folder
`scores/Beethoven,_Ludwig_van/6_Lieder,_Op.48/4_Die_Ehre_Gottes_aus_der_Natur/`), a transcription of
IMSLP #26415 by DanielR. Ludwig van Beethoven's music is in the public domain, and OpenScore
releases its transcriptions under CC0 1.0 (`<rights>OpenScore (CC0)</rights>` in the file). The
piece is for voice and piano: 716 notes, 43 bars, 80 BPM in 2/2, the piano on two staves.

| File | What it is | Made by |
| --- | --- | --- |
| `beethoven-op48-4-ehre-gottes.mxl` | compressed MusicXML (`lc5121134.mxl` in the corpus) | MuseScore 4.5.2 |
| `beethoven-op48-4-ehre-gottes-musescore3.mscz` | MuseScore 3 file (`lc5121134.mscz` in the corpus, unchanged) | MuseScore 3.6.2 |
| `beethoven-op48-4-ehre-gottes-musescore4.mscz` | the same score saved as a MuseScore 4 file | MuseScore 4.7.3: `mscore -o out.mscz beethoven-op48-4-ehre-gottes.mxl` |
| `beethoven-op48-4-ehre-gottes-musescore-export.mid` | MuseScore's own MIDI playback export of the `.mxl`, the independent reference for the notes | MuseScore 4.7.3: `mscore -o out.mid beethoven-op48-4-ehre-gottes.mxl` |

Downloaded 2026-10-12. `tests/musicxml_real_test.cpp` checks that every note of the `.mxl` has the
pitch, start tick and (within MuseScore's export gate) end tick of the MIDI export, and that both
`.mscz` files give the same notes as the `.mxl`. The MIDI export differs in two known ways: it
ends notes early (about 5%, half for staccato) and it plays one ornament in bar 6 as extra notes.

