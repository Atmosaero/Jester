# Running editor

Real captures of the preceding **red-accent Blue Jay iteration**, version **0.1.0-alpha.1**. The current Unreleased interface uses Blue Jay blue, icon-only modes/Loop, green MCP status and pointed translation arrows; these images have not been refreshed for that revision. The three mode images use the same synthetic character, pose, camera and authored materials. These images composite the native Atom framebuffer and Qt UI, including the SDK title bar and Jester logo; no rendered mockups are used. The English text entered in Russian search boxes is a user query, not an untranslated UI label. See [design sources](BLUE_JAY.md).

## Three viewport modes

Lit uses studio lighting, shadows and scene post effects.

![Lit](images/editor.png)

Unlit preserves base color/UV/alpha with neutral display conversion.

![Unlit](images/unlit.png)

Wireframe shows deformed triangle edges; skeleton display is disabled in these three captures.

![Mesh Wireframe](images/wireframe.png)

## Scene controls

Russian main-light controls with illuminance in lux and main-light shadows.

![Scene in Russian](images/scene-ru.png)

Studio environment contribution is independent of the visible background.

![Environment](images/environment.png)

Pipeline exposure, tone mapping and Bloom.

![Post effects](images/post.png)

## Preferences and localization

Existing appearance/overlay preferences in English and Russian.

![English preferences](images/preferences-en.png)

![Russian preferences](images/preferences-ru.png)

## Atom runtime identity

About reads the active RHI and compiled source identity. Runtime evidence also includes actual native-RHI registration logs, loaded Atom DLL paths and CPU/GPU output-buffer comparison; see [verification](VALIDATION.md) and [backend results](RENDER_BACKENDS.md).

![Atom DX12](images/atom-dx12.png)

The current development build reports Vulkan through the same runtime query. Validation of the earlier portable release remains documented separately in [packaging](PACKAGING.md).

![Atom Vulkan](images/atom-vulkan.png)
