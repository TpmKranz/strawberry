/*
   Strawberry Music Player
   This file was part of Clementine.
   Copyright 2004, Max Howell <max.howell@methylblue.com>
   Copyright 2009-2010, David Sansome <davidsansome@gmail.com>
   Copyright 2010, 2014, John Maguire <john.maguire@gmail.com>
   Copyright 2014-2015, Mark Furneaux <mark@furneaux.ca>
   Copyright 2014, Krzysztof Sobiecki <sobkas@gmail.com>

   Strawberry is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Strawberry is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Strawberry.  If not, see <http://www.gnu.org/licenses/>.

*/

#include "boomanalyzer.h"

#include <cmath>

#include <QWidget>
#include <QPixmap>
#include <QPainter>
#include <QPalette>
#include <QColor>

#include "engine/engine_fwd.h"
#include "engine/enginebase.h"
#include "fht.h"
#include "analyzerbase.h"

using Analyzer::Scope;

const int BoomAnalyzer::kColumnWidth = 4;
const int BoomAnalyzer::kMaxBandCount = 256;
const int BoomAnalyzer::kMinBandCount = 32;

const char *BoomAnalyzer::kName = QT_TRANSLATE_NOOP("AnalyzerContainer", "Boom analyzer");

BoomAnalyzer::BoomAnalyzer(QWidget *parent)
    : Analyzer::Base(parent, 9),
      bands_(0),
      scope_(kMinBandCount),
      fg_(palette().color(QPalette::Highlight)),
      K_barHeight_(1.271),  // 1.471
      F_peakSpeed_(1.103),  // 1.122
      F_(1.0),
      bar_height_(kMaxBandCount, 0),
      peak_height_(kMaxBandCount, 0),
      peak_speed_(kMaxBandCount, 0.01),
      barPixmap_(kColumnWidth, 50) {

  setMinimumWidth(kMinBandCount * (kColumnWidth + 1) - 1);
  setMaximumWidth(kMaxBandCount * (kColumnWidth + 1) - 1);

}

void BoomAnalyzer::changeK_barHeight(const int newValue) {
  K_barHeight_ = static_cast<double>(newValue) / 1000;
}

void BoomAnalyzer::changeF_peakSpeed(const int newValue) {
  F_peakSpeed_ = static_cast<double>(newValue) / 1000;
}

void BoomAnalyzer::resizeEvent(QResizeEvent *e) {

  QWidget::resizeEvent(e);

  const uint HEIGHT = height() - 2;
  const double h = 1.2 / HEIGHT;

  bands_ = qMin(static_cast<int>(static_cast<double>(width() + 1) / (kColumnWidth + 1)) + 1, kMaxBandCount);
  scope_.resize(bands_);

  F_ = double(HEIGHT) / (log10(256) * double(1.1) /*<- max. amplitude*/);

  barPixmap_ = QPixmap(kColumnWidth - 2, HEIGHT);
  canvas_ = QPixmap(size());
  canvas_.fill(palette().color(QPalette::Window));

  QPainter p(&barPixmap_);
  for (uint y = 0; y < HEIGHT; ++y) {
    const double F = static_cast<double>(y) * h;

    p.setPen(QColor(qMax(0, 255 - static_cast<int>(229.0 * F)),
                    qMax(0, 255 - static_cast<int>(229.0 * F)),
                    qMax(0, 255 - static_cast<int>(191.0 * F))));
    p.drawLine(0, y, kColumnWidth - 2, y);
  }

}

void BoomAnalyzer::transform(Scope &s) {

  fht_->spectrum(s.data());
  fht_->scale(s.data(), 1.0 / 50);

  s.resize(scope_.size() <= static_cast<quint64>(kMaxBandCount) / 2 ? kMaxBandCount / 2 : scope_.size());

}

void BoomAnalyzer::analyze(QPainter &p, const Scope &scope, const bool new_frame) {

  if (!new_frame || engine_->state() == Engine::Paused) {
    p.drawPixmap(0, 0, canvas_);
    return;
  }
  double h = 0.0;
  const uint MAX_HEIGHT = height() - 1;

  QPainter canvas_painter(&canvas_);
  canvas_.fill(palette().color(QPalette::Window));

  Analyzer::interpolate(scope, scope_);

  for (int i = 0, x = 0, y = 0; i < bands_; ++i, x += kColumnWidth + 1) {
    h = log10(scope_[i] * 256.0) * F_;

    if (h > MAX_HEIGHT) h = MAX_HEIGHT;

    if (h > bar_height_[i]) {
      bar_height_[i] = h;

      if (h > peak_height_[i]) {
        peak_height_[i] = h;
        peak_speed_[i] = 0.01;
      }
      else {
        goto peak_handling;
      }
    }
    else {
      if (bar_height_[i] > 0.0) {
        bar_height_[i] -= K_barHeight_;  // 1.4
        if (bar_height_[i] < 0.0) bar_height_[i] = 0.0;
      }

    peak_handling:

      if (peak_height_[i] > 0.0) {
        peak_height_[i] -= peak_speed_[i];
        peak_speed_[i] *= F_peakSpeed_;  // 1.12

        if (peak_height_[i] < bar_height_[i]) peak_height_[i] = bar_height_[i];
        if (peak_height_[i] < 0.0) peak_height_[i] = 0.0;
      }
    }

    y = height() - static_cast<int>(bar_height_[i]);
    canvas_painter.drawPixmap(x + 1, y, barPixmap_, 0, y, -1, -1);
    canvas_painter.setPen(fg_);
    if (bar_height_[i] > 0) {
      canvas_painter.drawRect(x, y, kColumnWidth - 1, height() - y - 1);
    }

    y = height() - static_cast<int>(peak_height_[i]);
    canvas_painter.setPen(palette().color(QPalette::Midlight));
    canvas_painter.drawLine(x, y, x + kColumnWidth - 1, y);
  }

  p.drawPixmap(0, 0, canvas_);

}
