# Viewport Math

For a source frame `srcW x srcH` and zoom percent `zoom`:

```text
roiW = clamp((srcW * 100) / zoom, minRoiW, srcW)
roiH = clamp((srcH * 100) / zoom, minRoiH, srcH)
maxX = srcW - roiW
maxY = srcH - roiH
roiX = clamp(roiX, 0, maxX)
roiY = clamp(roiY, 0, maxY)
```

The display scaler should map only the ROI to the destination panel.

Keep viewport state stable:

- Recompute ROI dimensions only when zoom or view mode changes.
- Keep `roiX` and `roiY` as integers.
- Use integer scaling where possible.
- Reset cached transform plans when zoom, source size, destination size, or view mode changes.
- Clear borders when switching between fit, crop, and zoom modes.
