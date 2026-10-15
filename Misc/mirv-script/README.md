# About

This folder has examples of how to use the HLAE `mirv_script` feature. Only CS2 is supported.

The feature runs JavaScript scripts in boajs, an ECMAScript engine. See the [limitations of boajs](https://boajs.dev/conformance).

In game, use `mirv_script_load` to load a script from a file and `mirv_script_exec` to execute inline.

Since HLAE 2.187.2, `mirv_script_load` accepts a relative path to the HLAE snippets folder at `path-to-HLAE\resources\AfxHookSource2\snippets`.

# Security warning

**It is very unsafe to execute _untrusted_ scripts**.

The scripting is meant to extend HLAE, so scripts can control your PC to a large degree.

# Installation

1. Clone repo
2. `cd Misc/mirv-script`
3. Install dependencies with `npm ci`
4. Run `npm run build` to transpile examples.

# Usage

1. Read the source code of the examples in `src/examples`. Also see the `types` folder, since the examples do not cover everything.
2. Run the examples from their respective folder in `dist` after transpiling.
3. Build your own scripts.

# Notes

- **Use an HLAE AfxHookSource2 Release build only.** A Debug build will crash (https://github.com/boa-dev/boa/issues/4089).
- Hooks can conflict, and one can overwrite another. Handle this yourself with a custom script.
- The global `mirv` object is the interface from the [rust implementation](https://github.com/advancedfx/advancedfx/blob/main/AfxHookSource2Rs/src/lib.rs). Its types are in `types/mirv.d.ts`.

Structure:
```
|
\---src
    |
    +---examples // Examples demonstrating some use cases.
    |
    +---snippets // Example snippets - currently unsorted and might conflict with each other and the src example (we need to make plugin system script still).
    |
    \---types // Description of mirv interface, etc.
```
