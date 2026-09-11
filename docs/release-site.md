# GitHub Pages release site

The DB1 release site lives in `site/`. It is plain HTML, CSS and JavaScript;
there is no frontend package install or build step and no native SDK dependency.
The Speckle logo is reused from the Speckle website.

Public site: <https://specklesystems.github.io/tekla-db1-sdk/>

## Local preview

From the repository root:

```sh
python3 -m http.server 4173 --bind 127.0.0.1
```

Open `http://127.0.0.1:4173/site/`. Previewing under a subdirectory exercises
the same relative asset URLs used by a GitHub Pages project site.

## Model demo

The supplied sample is **Sample Office Building (Tekla 2024 upgraded DB1)**.
The iframe and its companion link are pinned to:

- Server: `https://next.speckle.dev`
- Project: `426e88ed42`
- Model: `22261ad0a6`
- Version: `13cff6be58`

Project visibility was verified as `PUBLIC` through an unauthenticated GraphQL
request. No personal access token or embed share token is stored in the site.
The demo depends on the continued availability and public visibility of this
version. Check access while signed out before publishing or changing the model.
Update the iframe URL and companion link together when choosing another version.

The iframe displays hosted conversion output. The C++ SDK runs locally through
the quickstart commands; the site does not run a DB1 converter in the browser.
Model inputs, converted geometry, and validation screenshots remain outside the
repository, following `docs/repository-hygiene.md`.

## Publication

GitHub Pages is configured to deploy with GitHub Actions. Merging changes to
`site/` or its workflow into `main` publishes them automatically. For a manual
redeployment, run **Release site** from the Actions tab on `main`.

To configure the site again:

1. In this repository's **Settings → Pages**, select **GitHub Actions** as the
   source. The workflow does not change repository or model visibility.
2. Merge the site changes into `main`, or run **Release site** manually from
   `main` after enabling Pages. Pull requests validate the files without deploying.
3. Confirm the deployment completes and use its reported URL. The expected
   default project path is `/tekla-db1-sdk/`; an organization-level custom
   domain can change the hostname.
4. Verify desktop and mobile layout, the embedded model while signed out,
   navigation and clipboard buttons, and public access to the repository links.

Only `site/` is uploaded to Pages. The SDK source tree, local model files and
build outputs are not included in the deployment artifact. The download CTA
currently links to the source repository: add binary release links only once
those assets are actually published. Keep the early-alpha and support
copy aligned with the SDK release.
