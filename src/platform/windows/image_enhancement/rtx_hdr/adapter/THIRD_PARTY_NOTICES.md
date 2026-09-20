# Third-party build and distribution notice

The source code in this directory implements the Foundation Sunshine adapter for
the NVIDIA RTX Video SDK. NVIDIA SDK headers, import libraries, sample source,
and `nvngx_truehdr.dll` are not part of this repository.

Project-authored source in this directory is governed by the repository's
GPL-3.0 license. That license does not grant rights to NVIDIA materials.

Building this adapter requires a separately obtained NVIDIA RTX Video SDK and
acceptance of the terms supplied with that SDK. A resulting binary may include
or link NVIDIA SDK object code. Do not publish or redistribute that binary
unless the exact SDK version's distribution, attribution, end-user terms, and
notification requirements have been reviewed and satisfied alongside
Foundation Sunshine's GPL-3.0 obligations.

The DLL boundary and internal C ABI are compiler-interoperability mechanisms.
They do not by themselves establish license compatibility or permission to
redistribute NVIDIA materials.

References:

- NVIDIA RTX Video SDK: https://developer.nvidia.com/rtx-video-sdk/getting-started
- NVIDIA RTX SDKs License: https://developer.download.nvidia.com/gameworks/NVIDIA-RTX-SDKs-License-23Jan2023.pdf
- NVIDIA NGX Programming Guide: https://docs.nvidia.com/ngx/latest/programming-guide/
- NVIDIA software-use notification: https://developer.nvidia.com/sw-notification
- NVIDIA trademark policies: https://www.nvidia.com/en-us/about-nvidia/company-policies/

The license included with the SDK version actually used for a build controls
when it differs from these reference links.
