# Security policy

## Supported versions

Only the latest release receives security fixes.

## Reporting a vulnerability

Please do not open a public issue. Report privately through
[GitHub security advisories](https://github.com/DevnetraConsultancy/nee-download-manager/security/advisories/new)
or by email to devnetraconsultancy@gmail.com.

Include the version, a description, and steps or a proof of concept. You will get an
answer within a few days, and credit in the release notes if you want it.

## Design notes

- The browser bridge uses a named pipe restricted to SYSTEM and the current user; both ends
  verify each other's identity, and requests are validated before use.
- Downloaded files get Mark-of-the-Web so Windows SmartScreen checks them.
- Cookies handed over by the browser are stored encrypted with Windows DPAPI and are only
  sent to the site they belong to; redirects to other sites never carry them.
- DLLs load only from System32 and the application folder.
- The installer folder gets a protected ACL; file removal never follows junctions.
