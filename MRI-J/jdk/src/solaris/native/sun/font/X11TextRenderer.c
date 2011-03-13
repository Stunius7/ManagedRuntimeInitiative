/*
 * Copyright 2000-2005 Sun Microsystems, Inc.  All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Sun designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Sun in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 */

/*
 * Important note : All AWTxxx functions are defined in font.h.
 * These were added to remove the dependency of this file on X11.
 * These functions are used to perform X11 operations and should
 * be "stubbed out" in environments that do not support X11.
 * The implementation of these functions has been moved from this file
 * into X11TextRenderer_md.c, which is compiled into another library.
 */

#include "sun_font_X11TextRenderer.h"

#include "Region.h"
#include "SurfaceData.h"
#include "GraphicsPrimitiveMgr.h"
#include "glyphblitting.h"
#include "sunfontids.h"
#include <malloc.h>


JNIEXPORT void JNICALL AWTDrawGlyphList
(JNIEnv *env, jobject xtr,
 jlong dstData, jlong gc,
 SurfaceDataBounds *bounds, ImageRef *glyphs, jint totalGlyphs);

/*
 * Class:     sun_font_X11TextRenderer
 * Method:    doDrawGlyphList
 * Signature: (Lsun/java2d/SurfaceData;Ljava/awt/Rectangle;ILsun/font/GlyphList;J)V
 */
JNIEXPORT void JNICALL Java_sun_font_X11TextRenderer_doDrawGlyphList
    (JNIEnv *env, jobject xtr,
     jlong dstData, jlong xgc, jobject clip,
     jobject glyphlist)
{
    GlyphBlitVector* gbv;
    SurfaceDataBounds bounds;
    Region_GetBounds(env, clip, &bounds);

    if ((gbv = setupBlitVector(env, glyphlist)) == NULL) {
        return;
    }
    if (!RefineBounds(gbv, &bounds)) {
        free(gbv);
        return;
    }
    AWTDrawGlyphList(env, xtr, dstData, xgc,
                     &bounds, gbv->glyphs, gbv->numGlyphs);
    free(gbv);
}