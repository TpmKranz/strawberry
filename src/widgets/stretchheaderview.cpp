/*
 * Strawberry Music Player
 * This file was part of Clementine.
 * Copyright 2010, David Sansome <me@davidsansome.com>
 * Copyright 2018-2021, Jonas Kvinge <jonas@jkvinge.net>
 *
 * Strawberry is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Strawberry is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Strawberry.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "config.h"

#include <algorithm>
#include <numeric>

#include <QtGlobal>
#include <QWidget>
#include <QHeaderView>
#include <QAbstractItemModel>
#include <QIODevice>
#include <QArrayData>
#include <QByteArray>
#include <QDataStream>
#include <QList>
#include <QResizeEvent>
#include <QMouseEvent>

#include "stretchheaderview.h"

const int StretchHeaderView::kMinimumColumnWidth = 10;
const int StretchHeaderView::kMagicNumber = 0x502c950f;

StretchHeaderView::StretchHeaderView(const Qt::Orientation orientation, QWidget *parent)
    : QHeaderView(orientation, parent),
      stretch_enabled_(false),
      in_mouse_move_event_(false) {

  QObject::connect(this, &StretchHeaderView::sectionResized, this, &StretchHeaderView::SectionResized);
  setMinimumSectionSize(kMinimumColumnWidth);

}

void StretchHeaderView::setModel(QAbstractItemModel *model) {

  QHeaderView::setModel(model);

  if (stretch_enabled_) {
    column_widths_.resize(count());
    std::fill(column_widths_.begin(), column_widths_.end(), 1.0 / count());
  }

}

void StretchHeaderView::NormaliseWidths(const QList<int> &sections) {

  if (!stretch_enabled_) return;

  const ColumnWidthType total_sum = std::accumulate(column_widths_.begin(), column_widths_.end(), 0.0);
  ColumnWidthType selected_sum = total_sum;

  if (!sections.isEmpty()) {
    selected_sum = 0.0;
    for (int i = 0; i < count(); ++i)
      if (sections.contains(i)) {
        selected_sum += column_widths_[i];
      }
  }

  if (total_sum != 0.0 && !qFuzzyCompare(total_sum, 1.0)) {
    const ColumnWidthType mult = (selected_sum + (1.0 - total_sum)) / selected_sum;
    for (int i = 0; i < column_widths_.count(); ++i) {
      if (sections.isEmpty() || sections.contains(i)) {
        column_widths_[i] *= mult;
      }
    }
  }
}

void StretchHeaderView::UpdateWidths(const QList<int> &sections) {

  if (!stretch_enabled_) return;

  ColumnWidthType total_w = 0.0;

  for (int i = 0; i < column_widths_.count(); ++i) {
    const ColumnWidthType w = column_widths_[i];
    int pixels = w * width();

    total_w += w;

    if (!sections.isEmpty() && !sections.contains(i)) {
      continue;
    }

    if (pixels == 0 && !isSectionHidden(i)) {
      hideSection(i);
    }
    else if (pixels != 0 && isSectionHidden(i)) {
      showSection(i);
    }

    if (pixels != 0) {
      resizeSection(i, pixels);
    }
  }

}

void StretchHeaderView::HideSection(const int logical) {

  // Would this hide the last section?
  bool all_hidden = true;
  for (int i = 0; i < count(); ++i) {
    if (i != logical && !isSectionHidden(i) && sectionSize(i) > 0) {
      all_hidden = false;
      break;
    }
  }
  if (all_hidden) {
    return;
  }

  if (!stretch_enabled_) {
    hideSection(logical);
    return;
  }

  column_widths_[logical] = 0.0;
  NormaliseWidths();
  UpdateWidths();

}

void StretchHeaderView::ShowSection(int logical) {

  if (!stretch_enabled_) {
    showSection(logical);
    return;
  }

  // How many sections are visible already?
  int visible_count = 0;
  for (int i = 0; i < count(); ++i) {
    if (!isSectionHidden(i)) {
      ++visible_count;
    }
  }

  column_widths_[logical] = visible_count == 0 ? 1.0 : 1.0 / visible_count;
  NormaliseWidths();
  UpdateWidths();

}

void StretchHeaderView::SetSectionHidden(const int logical, const bool hidden) {

  if (hidden) {
    HideSection(logical);
  }
  else {
    ShowSection(logical);
  }

}

void StretchHeaderView::resizeEvent(QResizeEvent *e) {

  QHeaderView::resizeEvent(e);

  if (!stretch_enabled_) return;

  UpdateWidths();

}

void StretchHeaderView::mouseMoveEvent(QMouseEvent *e) {

  in_mouse_move_event_ = true;
  QHeaderView::mouseMoveEvent(e);
  in_mouse_move_event_ = false;

}

void StretchHeaderView::SectionResized(const int logical, const int, const int new_size) {

  if (!stretch_enabled_) return;

  if (in_mouse_move_event_) {
    // Update this section's proportional width
    column_widths_[logical] = ColumnWidthType(new_size) / width();

    // Find the visible sections to the right of the section that's being resized
    int visual = visualIndex(logical);
    QList<int> logical_sections_to_resize;
    for (int i = 0; i < count(); ++i) {
      if (!isSectionHidden(i) && visualIndex(i) > visual) {
        logical_sections_to_resize << i;
      }
    }

    // Resize just those columns
    if (!logical_sections_to_resize.isEmpty()) {
      in_mouse_move_event_ = false;
      UpdateWidths(logical_sections_to_resize);
      NormaliseWidths(logical_sections_to_resize);
      in_mouse_move_event_ = true;
    }
  }
}

void StretchHeaderView::ToggleStretchEnabled() {
  SetStretchEnabled(!is_stretch_enabled());
}

void StretchHeaderView::SetStretchEnabled(const bool enabled) {

  stretch_enabled_ = enabled;

  if (enabled) {
    // Initialize the list of widths from the current state of the widget
    column_widths_.resize(count());
    for (int i = 0; i < count(); ++i) {
      column_widths_[i] = ColumnWidthType(sectionSize(i)) / width();
    }

    // Stretch the columns to fill the widget
    NormaliseWidths();
    UpdateWidths();
  }

  emit StretchEnabledChanged(enabled);

}

void StretchHeaderView::SetColumnWidth(const int logical, const ColumnWidthType width) {

  if (!stretch_enabled_) return;

  column_widths_[logical] = width;

  QList<int> other_columns;
  for (int i = 0; i < count(); ++i) {
    if (!isSectionHidden(i) && i != logical) {
      other_columns << i;
    }
  }

  NormaliseWidths(other_columns);

}

bool StretchHeaderView::RestoreState(const QByteArray &sdata) {

  QDataStream s(sdata);
  s.setVersion(QDataStream::Qt_5_6);

  int magic_number = 0;
  s >> magic_number;

  if (magic_number != kMagicNumber || s.atEnd()) {
    return false;
  }

  QList<int> pixel_widths;
  QList<int> visual_indices;
  int sort_indicator_order = Qt::AscendingOrder;
  int sort_indicator_section = 0;

  s >> stretch_enabled_;
  s >> pixel_widths;
  s >> visual_indices;
  s >> column_widths_;
  s >> sort_indicator_order;
  s >> sort_indicator_section;

  setSortIndicator(sort_indicator_section, Qt::SortOrder(sort_indicator_order));

  const int persisted_column_count = qMin(qMin(visual_indices.count(), pixel_widths.count()), column_widths_.count());

  // Set column visible state, visual indices and, if we're not in stretch mode, pixel widths.
  for (int i = 0; i < count() && i < persisted_column_count; ++i) {
    setSectionHidden(i, pixel_widths[i] <= kMinimumColumnWidth);
    moveSection(visualIndex(visual_indices[i]), i);

    if (!stretch_enabled_) {
      resizeSection(i, pixel_widths[i]);
    }
  }

  // Have we added more columns since the last time?
  while (column_widths_.count() < count()) {
    column_widths_ << 0;
  }

  if (stretch_enabled_) {
    // In stretch mode, we've already set the proportional column widths so apply them now.
    UpdateWidths();
  }

  emit StretchEnabledChanged(stretch_enabled_);

  return true;

}

QByteArray StretchHeaderView::SaveState() const {

  QByteArray ret;
  QDataStream s(&ret, QIODevice::WriteOnly);

  QList<int> pixel_widths;
  QList<int> visual_indices;

  pixel_widths.reserve(count());
  visual_indices.reserve(count());

  for (int i = 0; i < count(); ++i) {
    pixel_widths << sectionSize(i);
    visual_indices << logicalIndex(i);
  }

  s.setVersion(QDataStream::Qt_5_6);

  s << kMagicNumber;

  s << stretch_enabled_;
  s << pixel_widths;
  s << visual_indices;
  s << column_widths_;
  s << int(sortIndicatorOrder());
  s << sortIndicatorSection();

  return ret;

}

QByteArray StretchHeaderView::ResetState() {

  QByteArray ret;

  QDataStream s(&ret, QIODevice::WriteOnly);
  s.setVersion(QDataStream::Qt_5_6);

  s << kMagicNumber;

  stretch_enabled_ = true;

  column_widths_.resize(count());
  std::fill(column_widths_.begin(), column_widths_.end(), 1.0 / count());

  QList<int> visual_indices;
  QList<int> pixel_widths;

  visual_indices.reserve(count());
  pixel_widths.reserve(count());

  for (int i = 0; i < count(); ++i) {
    pixel_widths << 10;
    visual_indices << count();
  }

  s << stretch_enabled_;
  s << pixel_widths;
  s << visual_indices;
  s << column_widths_;
  s << int(Qt::AscendingOrder);
  s << 0;

  RestoreState(ret);

  return ret;

}
