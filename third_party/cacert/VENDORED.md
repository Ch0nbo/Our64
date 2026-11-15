# CA certificates, vendored for Our64

`cacert.pem` is Mozilla's list of root certificates as PEM, extracted by the curl project:
https://curl.se/ca/cacert.pem, the version of 2026-11-05, SHA-256
427c064216761a1e72a8b30f0d954d08cb6e4435a82091fb865afd20c8497285, taken 2026-11-15.
License: MPL-2.0 (the certificate data is Mozilla's; see https://curl.se/docs/caextract.html).

Our64's updater checks GitHub's certificates against it (`player/source/update_wiiu.c`); it is
built into the app. The whole list and not only the roots GitHub uses today: an installed Our64
must still reach GitHub after GitHub changes its certificate authority.

To renew it: download the file again, check it against https://curl.se/ca/cacert.pem.sha256, and
change the date and the SHA-256 above.
