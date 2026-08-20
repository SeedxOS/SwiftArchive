# 7z and ZIPX fixtures

The committed fixtures let the test suite run without an installed archive
tool. Except for the two libarchive fixtures identified below, they were
created from project-owned test payloads with 7-Zip 26.02. Their redistribution
terms are in `LICENSE.generated-7zip-zipx`.

Representative generation commands are:

```sh
7zz a -t7z 7z-encrypted-content.7z input -popen-sesame -mhe=off -ms=on
7zz a -t7z 7z-encrypted-header.7z input -popen-sesame -mhe=on -ms=on
7zz a -t7z 7z-encrypted-unicode-password.7z input -p密碼-🔐 -mhe=on
7zz a -t7z 7z-split.7z input -v64b
7zz a -t7z 7z-encrypted-split.7z input -popen-sesame -mhe=on -v64b
7zz a -t7z 7z-encrypted-solid.7z solid -popen-sesame -mhe=on -ms=on
7zz a -t7z 7z-symlink.7z links -snl

7zz a -tzip zipx-deflate64.zipx zipx-payload.txt -mm=Deflate64
7zz a -tzip zipx-deflate64-aes.zipx zipx-payload.txt -mm=Deflate64 -mem=AES256 -popen-sesame
7zz a -tzip zipx-bzip2.zipx zipx-payload.txt -mm=BZip2
7zz a -tzip zipx-lzma.zipx zipx-payload.txt -mm=LZMA
7zz a -tzip zipx-ppmd.zipx zipx-payload.txt -mm=PPMd
7zz a -tzip zipx-mixed-methods.zipx deflate64.txt -mm=Deflate64
7zz a -tzip zipx-mixed-methods.zipx bzip2.txt -mm=BZip2
```

`zipx-zstd.zipx` and `zipx-xz.zipx` are decoded copies of these official
libarchive fixtures at commit `df40011ec353e38557e1ec5e1d45b4c2d368ad77`:

- `libarchive/test/test_read_format_zip_zstd.zipx.uu`
- `libarchive/test/test_read_format_zip_xz_multi.zipx.uu`

They remain covered by libarchive's distribution notice at
`Sources/CLibArchive/LICENSE.libarchive`.

## SHA-256

```text
0f714b2f1556138fdf37703d54d673d7f8d4a67d5e187d10268c39ce339f1af1  7z-encrypted-content.7z
685f778b6e228b9f48a57037423c5f08a27ded523f0d1ff1825842c79f10815d  7z-encrypted-header.7z
db64ae77cacf35dc094795225cc97eb36e25650cae8921aba77b0e06332d5b3b  7z-encrypted-solid.7z
616e37ed4b4a81f683c9770afcad36926e04e58749b9ba1bd7168beb359209ff  7z-encrypted-split.7z.001
eb08668f2bc7acfa56b4fa87225a14559b4873c230ca3940bacd5b217635f8db  7z-encrypted-split.7z.002
90ee7b27c36468e899c0c4aa6dd6084f22f60e0077b27a61b617b964c9198c0c  7z-encrypted-split.7z.003
29372342060f27bc2d436449a16a0992067c8f74c7cc4595c62103f42a1f65aa  7z-encrypted-split.7z.004
667cd4806e070a8ed363eeff5efc9cb948c3ae8fae93432c36e34402deffac14  7z-encrypted-unicode-password.7z
3283be734443a6f111f5ab824b23be28aebf778320f1a1c9f47c180ad1a46bf3  7z-split.7z.001
789390377eec7ed6eb6ecd1f656d1a3603682f1b482f8b97d30f2f40c43a92db  7z-split.7z.002
022fe37c7df1d2625be28de5fe2f80e02c8bacc30243f3bd7de15cbeb00d27a0  7z-split.7z.003
6e340b9cffb37a989ca544e6bb780a2c78901d3fb33738768511a30617afa01d  7z-split.7z.004
ce7ef1c1e2704a1d24ef68a3c97549496780fb87910c27fc2dab24fdeb0cf390  7z-symlink.7z
f135d959922e37505f6455fc933b0caf2cbc381ad81d14ea2bcee3c8b3d67f7f  zipx-bzip2.zipx
5a8bfa7de51677168e13d6deed1211261d48f69911788eb8fd5f252ff84d7825  zipx-deflate64-aes.zipx
ab2a85c18c1a2b284c7a8a6303e38aff0f0893f216001ccbf67826c6bfe454b0  zipx-deflate64.zipx
32d2c4d13c86f7249be52bea68c7d0f30cebf1834ab647485ac05156340483e4  zipx-lzma.zipx
e547ed990114e7d45324baaf1592043ba13dee80d70a03ce2395f32eb89150e9  zipx-mixed-methods.zipx
75105307d40a581e7b423865acab33f223a3c6f369662071c0e3d5af21efc265  zipx-payload.txt
2b33ced4bc194d27d668b9f2f2b668c87a2b757a9c044a26a2299f4c957f0a70  zipx-ppmd.zipx
d4ba4b1fa117e8170ffb4e0ded734c5da9006064daf35c9056671c66da21bb74  zipx-xz.zipx
ca15c7eabde8b918f1c91ff3a173012175b0599664f007fc3cb0bdf00c767377  zipx-zstd.zipx
```

Refresh this list with `shasum -a 256` whenever a fixture changes.
