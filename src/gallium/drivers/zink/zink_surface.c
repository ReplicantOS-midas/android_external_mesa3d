/*
 * Copyright 2018 Collabora Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "zink_context.h"
#include "zink_resource.h"
#include "zink_screen.h"
#include "zink_surface.h"

#include "util/format/u_format.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"

VkImageViewCreateInfo
create_ivci(struct zink_screen *screen,
            struct zink_resource *res,
            const struct pipe_surface *templ)
{
   VkImageViewCreateInfo ivci = {};
   ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
   ivci.image = res->obj->image;

   switch (res->base.target) {
   case PIPE_TEXTURE_1D:
      ivci.viewType = VK_IMAGE_VIEW_TYPE_1D;
      break;

   case PIPE_TEXTURE_1D_ARRAY:
      ivci.viewType = VK_IMAGE_VIEW_TYPE_1D_ARRAY;
      break;

   case PIPE_TEXTURE_2D:
   case PIPE_TEXTURE_RECT:
      ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
      break;

   case PIPE_TEXTURE_2D_ARRAY:
      ivci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
      break;

   case PIPE_TEXTURE_CUBE:
      ivci.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
      break;

   case PIPE_TEXTURE_CUBE_ARRAY:
      ivci.viewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
      break;

   case PIPE_TEXTURE_3D:
      ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
      break;

   default:
      unreachable("unsupported target");
   }

   ivci.format = zink_get_format(screen, templ->format);
   assert(ivci.format != VK_FORMAT_UNDEFINED);

   // TODO: format swizzles
   ivci.components.r = VK_COMPONENT_SWIZZLE_R;
   ivci.components.g = VK_COMPONENT_SWIZZLE_G;
   ivci.components.b = VK_COMPONENT_SWIZZLE_B;
   ivci.components.a = VK_COMPONENT_SWIZZLE_A;

   ivci.subresourceRange.aspectMask = res->aspect;
   ivci.subresourceRange.baseMipLevel = templ->u.tex.level;
   ivci.subresourceRange.levelCount = 1;
   ivci.subresourceRange.baseArrayLayer = templ->u.tex.first_layer;
   ivci.subresourceRange.layerCount = 1 + templ->u.tex.last_layer - templ->u.tex.first_layer;
   ivci.viewType = zink_surface_clamp_viewtype(ivci.viewType, templ->u.tex.first_layer, templ->u.tex.last_layer, res->base.array_size);

   return ivci;
}

static struct zink_surface *
create_surface(struct pipe_context *pctx,
               struct pipe_resource *pres,
               const struct pipe_surface *templ,
               VkImageViewCreateInfo *ivci)
{
   struct zink_screen *screen = zink_screen(pctx->screen);
   unsigned int level = templ->u.tex.level;

   struct zink_surface *surface = CALLOC_STRUCT(zink_surface);
   if (!surface)
      return NULL;

   pipe_resource_reference(&surface->base.texture, pres);
   pipe_reference_init(&surface->base.reference, 1);
   surface->base.context = pctx;
   surface->base.format = templ->format;
   surface->base.width = u_minify(pres->width0, level);
   surface->base.height = u_minify(pres->height0, level);
   surface->base.nr_samples = templ->nr_samples;
   surface->base.u.tex.level = level;
   surface->base.u.tex.first_layer = templ->u.tex.first_layer;
   surface->base.u.tex.last_layer = templ->u.tex.last_layer;

   if (vkCreateImageView(screen->dev, ivci, NULL,
                         &surface->image_view) != VK_SUCCESS) {
      FREE(surface);
      return NULL;
   }

   return surface;
}

static uint32_t
hash_ivci(const void *key)
{
   return _mesa_hash_data((char*)key + offsetof(VkImageViewCreateInfo, flags), sizeof(VkImageViewCreateInfo) - offsetof(VkImageViewCreateInfo, flags));
}

struct pipe_surface *
zink_get_surface(struct zink_context *ctx,
            struct pipe_resource *pres,
            const struct pipe_surface *templ,
            VkImageViewCreateInfo *ivci)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   struct zink_surface *surface = NULL;
   uint32_t hash = hash_ivci(ivci);

   simple_mtx_lock(&screen->surface_mtx);
   struct hash_entry *entry = _mesa_hash_table_search_pre_hashed(&screen->surface_cache, hash, ivci);

   if (!entry) {
      /* create a new surface */
      surface = create_surface(&ctx->base, pres, templ, ivci);
      surface->hash = hash;
      surface->ivci = *ivci;
      entry = _mesa_hash_table_insert_pre_hashed(&screen->surface_cache, hash, &surface->ivci, surface);
      if (!entry) {
         simple_mtx_unlock(&screen->surface_mtx);
         return NULL;
      }

      surface = entry->data;
   } else {
      surface = entry->data;
      p_atomic_inc(&surface->base.reference.count);
   }
   simple_mtx_unlock(&screen->surface_mtx);

   return &surface->base;
}

static struct pipe_surface *
zink_create_surface(struct pipe_context *pctx,
                    struct pipe_resource *pres,
                    const struct pipe_surface *templ)
{

   VkImageViewCreateInfo ivci = create_ivci(zink_screen(pctx->screen),
                                            zink_resource(pres), templ);

   return zink_get_surface(zink_context(pctx), pres, templ, &ivci);
}

static void
zink_surface_destroy(struct pipe_context *pctx,
                     struct pipe_surface *psurface)
{
   struct zink_screen *screen = zink_screen(pctx->screen);
   struct zink_surface *surface = zink_surface(psurface);
   simple_mtx_lock(&screen->surface_mtx);
   struct hash_entry *he = _mesa_hash_table_search_pre_hashed(&screen->surface_cache, surface->hash, &surface->ivci);
   assert(he);
   _mesa_hash_table_remove(&screen->surface_cache, he);
   simple_mtx_unlock(&screen->surface_mtx);
   pipe_resource_reference(&psurface->texture, NULL);
   vkDestroyImageView(screen->dev, surface->image_view, NULL);
   FREE(surface);
}

void
zink_context_surface_init(struct pipe_context *context)
{
   context->create_surface = zink_create_surface;
   context->surface_destroy = zink_surface_destroy;
}
