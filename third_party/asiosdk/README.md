# ASIO interface headers

Unmodified `common/asio.h`, `common/asiosys.h`, `common/iasiodrv.h` and
`common/LICENSE.txt` from Steinberg ASIO SDK 2.3.4 (2025-10-15), copied from
the local NereusSDR reference at `3e5bf0b741d5e7bb311e6b24dd08b62a97efedcd`.
AetherSDR uses these interfaces under the SDK's GPL version 3 option.
No SDK host implementation, driver, logo or binary is included.

Nereus records archive SHA-256
`d5ebf0c20dd2c5f43771fd0c1418f4b361bf52434ee670097cfa6b3a335e2eca`.
The copied headers are used only in the Windows implementation of
`src/core/AsioOutput.cpp`. The application supplies its own output-only host.
