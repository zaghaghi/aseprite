// Aseprite
// Copyright (C) 2019-2020  Igara Studio S.A.
// Copyright (C) 2001-2017  David Capello
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#include "app/util/wrap_point.h"

#include "app/tools/ink.h"

namespace app {
namespace tools {

class NonePointShape : public PointShape {
public:
  void transformPoint(ToolLoop* loop, int x, int y) override {
    // Do nothing
  }

  void getModifiedArea(ToolLoop* loop, int x, int y, Rect& area) override {
    // Do nothing
  }
};

class PixelPointShape : public PointShape {
public:
  bool isPixel() override { return true; }

  void transformPoint(ToolLoop* loop, int x, int y) override {
    doInkHline(x, y, x, loop);
  }

  void getModifiedArea(ToolLoop* loop, int x, int y, Rect& area) override {
    area = Rect(x, y, 1, 1);
  }
};

class BrushPointShape : public PointShape {
  Brush* m_lastBrush;
  std::shared_ptr<CompressedImage> m_compressedImage;
  bool m_firstPoint;

public:

  void preparePointShape(ToolLoop* loop) override {
    m_firstPoint = true;
    m_lastBrush = nullptr;
  }

  void transformPoint(ToolLoop* loop, int x, int y) override {
    Brush* m_brush = loop->getBrush();
    if (m_lastBrush != m_brush) {
      m_lastBrush = m_brush;
      m_compressedImage.reset(new CompressedImage(m_brush->image(),
                                                  m_brush->maskBitmap(),
                                                  false));
    }

    x += m_brush->bounds().x;
    y += m_brush->bounds().y;

    if (m_firstPoint) {
      if ((m_brush->type() == kImageBrushType) &&
          (m_brush->pattern() == BrushPattern::ALIGNED_TO_DST ||
           m_brush->pattern() == BrushPattern::PAINT_BRUSH)) {
        m_brush->setPatternOrigin(gfx::Point(x, y));
      }
    }
    else {
      if (m_brush->type() == kImageBrushType &&
          m_brush->pattern() == BrushPattern::PAINT_BRUSH) {
        m_brush->setPatternOrigin(gfx::Point(x, y));
      }
    }

    if (int(loop->getTiledMode()) & int(TiledMode::X_AXIS)) {
      int wrappedPatternOriginX = wrap_value(m_brush->patternOrigin().x, loop->sprite()->width()) % m_brush->bounds().w;
      m_brush->setPatternOrigin(gfx::Point(wrappedPatternOriginX, m_brush->patternOrigin().y));
      x = wrap_value(x, loop->sprite()->width());
    }
    if (int(loop->getTiledMode()) & int(TiledMode::Y_AXIS)) {
      int wrappedPatternOriginY = wrap_value(m_brush->patternOrigin().y, loop->sprite()->height()) % m_brush->bounds().h;
      m_brush->setPatternOrigin(gfx::Point(m_brush->patternOrigin().x, wrappedPatternOriginY));
      y = wrap_value(y, loop->sprite()->height());
    }

    loop->getInk()->prepareForPointShape(loop, m_firstPoint, x, y);

    for (auto scanline : *m_compressedImage) {
      int u = x+scanline.x;
      loop->getInk()->prepareVForPointShape(loop, y+scanline.y);
      doInkHline(u, y+scanline.y, u+scanline.w-1, loop);
    }
    m_firstPoint = false;
  }

  void getModifiedArea(ToolLoop* loop, int x, int y, Rect& area) override {
    area = loop->getBrush()->bounds();
    area.x += x;
    area.y += y;
  }

};

class FloodFillPointShape : public PointShape {
public:
  bool isFloodFill() override { return true; }

  void transformPoint(ToolLoop* loop, int x, int y) override {
    const doc::Image* srcImage = loop->getFloodFillSrcImage();
    gfx::Point pt = wrap_point(loop->getTiledMode(),
                               gfx::Size(srcImage->width(),
                                         srcImage->height()),
                               gfx::Point(x, y), true);

    doc::algorithm::floodfill(
      srcImage,
      (loop->useMask() ? loop->getMask(): nullptr),
      pt.x, pt.y,
      floodfillBounds(loop, pt.x, pt.y),
      get_pixel(srcImage, pt.x, pt.y),
      loop->getTolerance(),
      loop->getContiguous(),
      loop->isPixelConnectivityEightConnected(),
      loop, (AlgoHLine)doInkHline);
  }

  void getModifiedArea(ToolLoop* loop, int x, int y, Rect& area) override {
    area = floodfillBounds(loop, x, y);
  }

private:
  gfx::Rect floodfillBounds(ToolLoop* loop, int x, int y) const {
    gfx::Rect bounds = loop->sprite()->bounds();
    bounds &= loop->getFloodFillSrcImage()->bounds();

    // Limit the flood-fill to the current tile if the grid is visible.
    if (loop->getStopAtGrid()) {
      gfx::Rect grid = loop->getGridBounds();
      if (!grid.isEmpty()) {
        div_t d, dx, dy;

        dx = div(grid.x, grid.w);
        dy = div(grid.y, grid.h);

        if (dx.rem > 0) dx.rem -= grid.w;
        if (dy.rem > 0) dy.rem -= grid.h;

        d = div(x-dx.rem, grid.w);
        x = dx.rem + d.quot*grid.w;

        d = div(y-dy.rem, grid.h);
        y = dy.rem + d.quot*grid.h;

        bounds = bounds.createIntersection(gfx::Rect(x, y, grid.w, grid.h));
      }
    }

    return bounds;
  }
};

class SprayPointShape : public PointShape {
  BrushPointShape m_subPointShape;
  float m_pointRemainder = 0;

public:

  bool isSpray() override { return true; }

  void preparePointShape(ToolLoop* loop) override {
    m_subPointShape.preparePointShape(loop);
  }

  void transformPoint(ToolLoop* loop, int x, int y) override {
    int spray_width = loop->getSprayWidth();
    int spray_speed = loop->getSpraySpeed();

    // The number of points to spray is proportional to the spraying area, and
    // we calculate it as a float to handle very low spray rates properly.
    float points_to_spray = (spray_width * spray_width / 4.0f) * spray_speed / 100.0f;

    // We add the fractional points from last time to get
    // the total number of points to paint this time.
    points_to_spray += m_pointRemainder;
    int integral_points = (int)points_to_spray;

    // Save any leftover fraction of a point for next time.
    m_pointRemainder = points_to_spray - integral_points;
    ASSERT(m_pointRemainder >= 0 && m_pointRemainder < 1.0f);

    fixmath::fixed angle, radius;

    for (int c=0; c<integral_points; c++) {

#if RAND_MAX <= 0xffff
      // In Windows, rand() has a RAND_MAX too small
      angle = fixmath::itofix(rand() * 255 / RAND_MAX);
      radius = fixmath::itofix(rand() * spray_width / RAND_MAX);
#else
      angle = rand();
      radius = rand() % fixmath::itofix(spray_width);
#endif

      int u = fixmath::fixtoi(fixmath::fixmul(radius, fixmath::fixcos(angle)));
      int v = fixmath::fixtoi(fixmath::fixmul(radius, fixmath::fixsin(angle)));
      m_subPointShape.transformPoint(loop, x+u, y+v);
    }
  }

  void getModifiedArea(ToolLoop* loop, int x, int y, Rect& area) override {
    int spray_width = loop->getSprayWidth();
    Point p1(x-spray_width, y-spray_width);
    Point p2(x+spray_width, y+spray_width);

    Rect area1;
    Rect area2;
    m_subPointShape.getModifiedArea(loop, p1.x, p1.y, area1);
    m_subPointShape.getModifiedArea(loop, p2.x, p2.y, area2);

    area = area1.createUnion(area2);
  }
};

} // namespace tools
} // namespace app
