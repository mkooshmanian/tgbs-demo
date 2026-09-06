# Minimal headless X stack
PACKAGECONFIG:append = " xvfb"
PACKAGECONFIG:remove = " dri dri3 glx glamor"
