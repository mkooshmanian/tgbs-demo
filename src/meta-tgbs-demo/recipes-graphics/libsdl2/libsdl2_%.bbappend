# Keep SDL2 on a pure X11 backend for the Xvfb/x11vnc deployment used by
# KosmoDoom. This policy belongs to the product layer, not to meta-doom.
PACKAGECONFIG:append:class-target = " x11"
PACKAGECONFIG:remove:class-target = "directfb gles2 kmsdrm opengl vulkan wayland"
