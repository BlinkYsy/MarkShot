# MarkShot English OCR runtime (Windows x64)

Native x64 DLLs from https://www.nuget.org/packages/Tesseract/5.2.0
(https://github.com/charlesw/tesseract). No .NET wrapper is loaded.
TessVersion reports **5.0.0**; the NuGet package version is **5.2.0**.
Leptonica is **1.82.0**.

- Tesseract: Apache-2.0, see LICENSE-Tesseract.txt.
- Leptonica: BSD-style license, see LICENSE-Leptonica.txt.
- English LSTM model: https://github.com/tesseract-ocr/tessdata_fast,
  Apache-2.0, see LICENSE-tessdata.txt. Downloaded 2026-10-06.

SHA-256:

```text
tesseract50.dll
DE4D04EC75095374D98F5DD7A60D14D7E2E0F76589DB693ECCF7AE658BE8CB2B
leptonica-1.82.0.dll
DFCB3E6ED0B16BC55BFDBCF53543CFE42A354B87C3E35BD3A95EEBF005D73E76
tessdata/eng.traineddata
7D4322BD2A7749724879683FC3912CB542F19906C83BCC1A52132556427170B2
```

The build copies the required Microsoft VC++ x64 redistributable DLLs from
Visual Studio's VC/Redist directory to the `ocr` folder when available.
These Microsoft components remain subject to the Visual Studio redistribution
license. Windows 10/11 supplies the Universal CRT and other system dependencies.

The model loader uses the system ANSI code page. Chinese paths work on the
current Chinese Windows system. For characters outside that code page, move
the application folder to an ASCII-only path.
