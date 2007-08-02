/*
 * Copyright (C) 2007 Google (Evan Stade)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "windef.h"
#include "wingdi.h"

#include "objbase.h"

#include "gdiplus.h"
#include "gdiplus_private.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(gdiplus);

GpStatus WINGDIPAPI GdipCloneBrush(GpBrush *brush, GpBrush **clone)
{
    if(!brush || !clone)
        return InvalidParameter;

    switch(brush->bt){
        case BrushTypeSolidColor:
            *clone = GdipAlloc(sizeof(GpSolidFill));
            if (!*clone) return OutOfMemory;

            memcpy(*clone, brush, sizeof(GpSolidFill));

            (*clone)->gdibrush = CreateBrushIndirect(&(*clone)->lb);
            break;
        default:
            return NotImplemented;
    }

    return Ok;
}

/* FIXME: path gradient brushes not truly supported (drawn as solid brushes) */
GpStatus WINGDIPAPI GdipCreatePathGradientFromPath(GDIPCONST GpPath* path,
    GpPathGradient **grad)
{
    COLORREF col = ARGB2COLORREF(0xffffffff);

    if(!path || !grad)
        return InvalidParameter;

    *grad = GdipAlloc(sizeof(GpPathGradient));
    if (!*grad) return OutOfMemory;

    (*grad)->brush.lb.lbStyle = BS_SOLID;
    (*grad)->brush.lb.lbColor = col;
    (*grad)->brush.lb.lbHatch = 0;

    (*grad)->brush.gdibrush = CreateSolidBrush(col);
    (*grad)->brush.bt = BrushTypeSolidColor;
    (*grad)->centercolor = 0xffffffff;
    (*grad)->wrap = WrapModeClamp;

    return Ok;
}

GpStatus WINGDIPAPI GdipCreateSolidFill(ARGB color, GpSolidFill **sf)
{
    COLORREF col = ARGB2COLORREF(color);

    if(!sf)  return InvalidParameter;

    *sf = GdipAlloc(sizeof(GpSolidFill));
    if (!*sf) return OutOfMemory;

    (*sf)->brush.lb.lbStyle = BS_SOLID;
    (*sf)->brush.lb.lbColor = col;
    (*sf)->brush.lb.lbHatch = 0;

    (*sf)->brush.gdibrush = CreateSolidBrush(col);
    (*sf)->brush.bt = BrushTypeSolidColor;
    (*sf)->color = color;

    return Ok;
}

GpStatus WINGDIPAPI GdipGetBrushType(GpBrush *brush, GpBrushType *type)
{
    if(!brush || !type)  return InvalidParameter;

    *type = brush->bt;

    return Ok;
}

GpStatus WINGDIPAPI GdipDeleteBrush(GpBrush *brush)
{
    if(!brush)  return InvalidParameter;

    DeleteObject(brush->gdibrush);
    GdipFree(brush);

    return Ok;
}

GpStatus WINGDIPAPI GdipGetSolidFillColor(GpSolidFill *sf, ARGB *argb)
{
    if(!sf || !argb)
        return InvalidParameter;

    *argb = sf->color;

    return Ok;
}

GpStatus WINGDIPAPI GdipSetPathGradientCenterColor(GpPathGradient *grad,
    ARGB argb)
{
    if(!grad)
        return InvalidParameter;

    grad->centercolor = argb;
    grad->brush.lb.lbColor = ARGB2COLORREF(argb);

    DeleteObject(grad->brush.gdibrush);
    grad->brush.gdibrush = CreateSolidBrush(grad->brush.lb.lbColor);

    return Ok;
}

GpStatus WINGDIPAPI GdipSetPathGradientWrapMode(GpPathGradient *grad,
    GpWrapMode wrap)
{
    if(!grad)
        return InvalidParameter;

    grad->wrap = wrap;

    return Ok;
}

GpStatus WINGDIPAPI GdipSetSolidFillColor(GpSolidFill *sf, ARGB argb)
{
    if(!sf)
        return InvalidParameter;

    sf->color = argb;
    sf->brush.lb.lbColor = ARGB2COLORREF(argb);

    DeleteObject(sf->brush.gdibrush);
    sf->brush.gdibrush = CreateSolidBrush(sf->brush.lb.lbColor);

    return Ok;
}
