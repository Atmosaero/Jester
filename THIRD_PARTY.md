# Third-party software and assets

Jester is MIT-licensed and derives from [Trickster](https://github.com/Atmosaero/Trickster), source snapshot `b9f10524aa2cc9056f1cf8d2627eebc3f9244aaa`. Original copyright notices are retained in [LICENSE](LICENSE).

| Component | Use | License / notices |
|---|---|---|
| [O3DE](https://github.com/o3de/o3de) | Atom renderer and application framework; installed separately | Apache-2.0 OR MIT; [license](licenses/O3DE-LICENSE.txt), [notices](licenses/O3DE-NOTICES.txt) |
| [Qt](https://www.qt.io/licensing/) | UI; supplied by the installed O3DE SDK | Qt's applicable LGPL/GPL or commercial terms; consult the SDK distribution |
| [ufbx](https://github.com/ufbx/ufbx) | Vendored FBX importer | [MIT OR public domain](Editor/third_party/ufbx/LICENSE); [upstream revision](Editor/third_party/ufbx/UPSTREAM.txt) |
| [MuJoCo 3.3.7](https://github.com/google-deepmind/mujoco) | Downloaded by CMake and dynamically linked | [Apache-2.0](licenses/MuJoCo-LICENSE.txt), [notices](licenses/MuJoCo-THIRD_PARTY_NOTICES.txt) |

Material extension points and property groups in `Assets/Materials/Types` derive from O3DE; their copyright/SPDX notices are retained. `tools/prepare_materials.py` reproduces these adaptations from an installed SDK. [Atom SampleViewer](https://github.com/o3de/o3de-atom-sampleviewer/tree/03ec83468dfdd4281c8fa73be0dea5fb171015af) served as an API reference.

The synthetic mannequin, examples and documentation screenshots are original project content under MIT. No Epic mannequin, Juri reference, private character library, SDK runtime or generated video is distributed in this repository. Assets imported by users retain their own licenses.
