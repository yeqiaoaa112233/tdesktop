/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/view/media/history_view_media_grouped.h"

#include "history/history_item_components.h"
#include "history/history_message.h"
#include "history/history.h"
#include "history/view/history_view_element.h"
#include "history/view/history_view_cursor_state.h"
#include "data/data_media_types.h"
#include "data/data_session.h"
#include "storage/storage_shared_media.h"
#include "lang/lang_keys.h"
#include "ui/grouped_layout.h"
#include "ui/text/text_options.h"
#include "layout.h"
#include "styles/style_chat.h"

namespace HistoryView {
namespace {

std::vector<Ui::GroupMediaLayout> LayoutPlaylist(
		const std::vector<QSize> &sizes) {
	Expects(!sizes.empty());

	auto result = std::vector<Ui::GroupMediaLayout>();
	result.reserve(sizes.size());
	const auto width = ranges::max_element(
		sizes,
		std::less<>(),
		&QSize::width)->width();
	auto top = 0;
	for (const auto &size : sizes) {
		result.push_back({
			.geometry = QRect(0, top, width, size.height()),
			.sides = RectPart::Left | RectPart::Right
		});
		top += size.height();
	}
	result.front().sides |= RectPart::Top;
	result.back().sides |= RectPart::Bottom;
	return result;
}

} // namespace

GroupedMedia::Part::Part(
	not_null<Element*> parent,
	not_null<Data::Media*> media)
: item(media->parent())
, content(media->createView(parent, item)) {
	Assert(media->canBeGrouped());
}

GroupedMedia::GroupedMedia(
	not_null<Element*> parent,
	const std::vector<std::unique_ptr<Data::Media>> &medias)
: Media(parent)
, _caption(st::minPhotoSize - st::msgPadding.left() - st::msgPadding.right()) {
	const auto truncated = ranges::view::all(
		medias
	) | ranges::view::transform([](const std::unique_ptr<Data::Media> &v) {
		return v.get();
	}) | ranges::view::take(kMaxSize);
	const auto result = applyGroup(truncated);

	Ensures(result);
}

GroupedMedia::GroupedMedia(
	not_null<Element*> parent,
	const std::vector<not_null<HistoryItem*>> &items)
: Media(parent)
, _caption(st::minPhotoSize - st::msgPadding.left() - st::msgPadding.right()) {
	const auto medias = ranges::view::all(
		items
	) | ranges::view::transform([](not_null<HistoryItem*> item) {
		return item->media();
	}) | ranges::view::take(kMaxSize);
	const auto result = applyGroup(medias);

	Ensures(result);
}

GroupedMedia::~GroupedMedia() {
	// Destroy all parts while the media object is still not destroyed.
	base::take(_parts);
}

GroupedMedia::Mode GroupedMedia::DetectMode(not_null<Data::Media*> media) {
	const auto document = media->document();
	return (document && !document->isVideoFile())
		? Mode::Column
		: Mode::Grid;
}

QSize GroupedMedia::countOptimalSize() {
	if (_caption.hasSkipBlock()) {
		_caption.updateSkipBlock(
			_parent->skipBlockWidth(),
			_parent->skipBlockHeight());
	}

	std::vector<QSize> sizes;
	const auto partsCount = _parts.size();
	sizes.reserve(partsCount);
	auto maxWidth = 0;
	if (_mode == Mode::Column) {
		for (const auto &part : _parts) {
			const auto &media = part.content;
			media->initDimensions();
			accumulate_max(maxWidth, media->maxWidth());
		}
	}
	auto index = 0;
	for (const auto &part : _parts) {
		const auto last = (++index == partsCount);
		sizes.push_back(part.content->sizeForGroupingOptimal(maxWidth, last));
	}

	const auto layout = (_mode == Mode::Grid)
		? Ui::LayoutMediaGroup(
			sizes,
			st::historyGroupWidthMax,
			st::historyGroupWidthMin,
			st::historyGroupSkip)
		: LayoutPlaylist(sizes);
	Assert(layout.size() == _parts.size());

	auto minHeight = 0;
	for (auto i = 0, count = int(layout.size()); i != count; ++i) {
		const auto &item = layout[i];
		accumulate_max(maxWidth, item.geometry.x() + item.geometry.width());
		accumulate_max(minHeight, item.geometry.y() + item.geometry.height());
		_parts[i].initialGeometry = item.geometry;
		_parts[i].sides = item.sides;
	}

	if (!_caption.isEmpty()) {
		auto captionw = maxWidth - st::msgPadding.left() - st::msgPadding.right();
		minHeight += st::mediaCaptionSkip + _caption.countHeight(captionw);
		if (isBubbleBottom()) {
			minHeight += st::msgPadding.bottom();
		}
	}
	return { maxWidth, minHeight };
}

QSize GroupedMedia::countCurrentSize(int newWidth) {
	accumulate_min(newWidth, maxWidth());
	auto newHeight = 0;
	if (_mode == Mode::Grid && newWidth < st::historyGroupWidthMin) {
		return { newWidth, newHeight };
	} else if (_mode == Mode::Column) {
		auto index = 0;
		auto top = 0;
		for (auto &part : _parts) {
			const auto last = (++index == _parts.size());
			const auto size = part.content->sizeForGrouping(newWidth, last);
			part.geometry = QRect(0, top, newWidth, size.height());
			top += size.height();
		}
		newHeight = top;
	} else {
		const auto initialSpacing = st::historyGroupSkip;
		const auto factor = newWidth / float64(maxWidth());
		const auto scale = [&](int value) {
			return int(std::round(value * factor));
		};
		const auto spacing = scale(initialSpacing);
		for (auto &part : _parts) {
			const auto sides = part.sides;
			const auto initialGeometry = part.initialGeometry;
			const auto needRightSkip = !(sides & RectPart::Right);
			const auto needBottomSkip = !(sides & RectPart::Bottom);
			const auto initialLeft = initialGeometry.x();
			const auto initialTop = initialGeometry.y();
			const auto initialRight = initialLeft
				+ initialGeometry.width()
				+ (needRightSkip ? initialSpacing : 0);
			const auto initialBottom = initialTop
				+ initialGeometry.height()
				+ (needBottomSkip ? initialSpacing : 0);
			const auto left = scale(initialLeft);
			const auto top = scale(initialTop);
			const auto width = scale(initialRight)
				- left
				- (needRightSkip ? spacing : 0);
			const auto height = scale(initialBottom)
				- top
				- (needBottomSkip ? spacing : 0);
			part.geometry = QRect(left, top, width, height);

			accumulate_max(newHeight, top + height);
		}
	}
	if (!_caption.isEmpty()) {
		const auto captionw = newWidth - st::msgPadding.left() - st::msgPadding.right();
		newHeight += st::mediaCaptionSkip + _caption.countHeight(captionw);
		if (isBubbleBottom()) {
			newHeight += st::msgPadding.bottom();
		}
	}

	return { newWidth, newHeight };
}

void GroupedMedia::refreshParentId(
		not_null<HistoryItem*> realParent) {
	for (const auto &part : _parts) {
		part.content->refreshParentId(part.item);
	}
}

RectParts GroupedMedia::cornersFromSides(RectParts sides) const {
	auto result = Ui::GetCornersFromSides(sides);
	if (!isBubbleTop()) {
		result &= ~(RectPart::TopLeft | RectPart::TopRight);
	}
	if (!isRoundedInBubbleBottom() || !_caption.isEmpty()) {
		result &= ~(RectPart::BottomLeft | RectPart::BottomRight);
	}
	return result;
}

void GroupedMedia::draw(
		Painter &p,
		const QRect &clip,
		TextSelection selection,
		crl::time ms) const {
	for (auto i = 0, count = int(_parts.size()); i != count; ++i) {
		const auto &part = _parts[i];
		const auto partSelection = (selection == FullSelection)
			? FullSelection
			: IsGroupItemSelection(selection, i)
			? FullSelection
			: TextSelection();
		const auto last = (i + 1 == count);
		part.content->drawGrouped(
			p,
			clip,
			partSelection,
			ms,
			part.geometry,
			part.sides,
			cornersFromSides(part.sides),
			&part.cacheKey,
			&part.cache,
			last);
	}

	// date
	const auto selected = (selection == FullSelection);
	if (!_caption.isEmpty()) {
		const auto captionw = width() - st::msgPadding.left() - st::msgPadding.right();
		const auto outbg = _parent->hasOutLayout();
		const auto captiony = height()
			- (isBubbleBottom() ? st::msgPadding.bottom() : 0)
			- _caption.countHeight(captionw);
		p.setPen(outbg ? (selected ? st::historyTextOutFgSelected : st::historyTextOutFg) : (selected ? st::historyTextInFgSelected : st::historyTextInFg));
		_caption.draw(p, st::msgPadding.left(), captiony, captionw, style::al_left, 0, -1, selection);
	} else if (_parent->media() == this) {
		auto fullRight = width();
		auto fullBottom = height();
		if (needInfoDisplay()) {
			_parent->drawInfo(p, fullRight, fullBottom, width(), selected, InfoDisplayType::Image);
		}
		if (const auto size = _parent->hasBubble() ? std::nullopt : _parent->rightActionSize()) {
			auto fastShareLeft = (fullRight + st::historyFastShareLeft);
			auto fastShareTop = (fullBottom - st::historyFastShareBottom - size->height());
			_parent->drawRightAction(p, fastShareLeft, fastShareTop, width());
		}
	}
}

TextState GroupedMedia::getPartState(
		QPoint point,
		StateRequest request) const {
	auto index = 0;
	for (const auto &part : _parts) {
		++index;
		if (part.geometry.contains(point)) {
			const auto last = (index == _parts.size());
			auto result = part.content->getStateGrouped(
				part.geometry,
				part.sides,
				point,
				request,
				last);
			result.itemId = part.item->fullId();
			return result;
		}
	}
	return TextState(_parent->data());
}

PointState GroupedMedia::pointState(QPoint point) const {
	if (!QRect(0, 0, width(), height()).contains(point)) {
		return PointState::Outside;
	}
	for (const auto &part : _parts) {
		if (part.geometry.contains(point)) {
			return PointState::GroupPart;
		}
	}
	return PointState::Inside;
}

TextState GroupedMedia::textState(QPoint point, StateRequest request) const {
	auto result = getPartState(point, request);
	if (!result.link && !_caption.isEmpty()) {
		const auto captionw = width() - st::msgPadding.left() - st::msgPadding.right();
		const auto captiony = height()
			- (isBubbleBottom() ? st::msgPadding.bottom() : 0)
			- _caption.countHeight(captionw);
		if (QRect(st::msgPadding.left(), captiony, captionw, height() - captiony).contains(point)) {
			return TextState(_parent->data(), _caption.getState(
				point - QPoint(st::msgPadding.left(), captiony),
				captionw,
				request.forText()));
		}
	} else if (_parent->media() == this) {
		auto fullRight = width();
		auto fullBottom = height();
		if (_parent->pointInTime(fullRight, fullBottom, point, InfoDisplayType::Image)) {
			result.cursor = CursorState::Date;
		}
		if (const auto size = _parent->hasBubble() ? std::nullopt : _parent->rightActionSize()) {
			auto fastShareLeft = (fullRight + st::historyFastShareLeft);
			auto fastShareTop = (fullBottom - st::historyFastShareBottom - size->height());
			if (QRect(fastShareLeft, fastShareTop, size->width(), size->height()).contains(point)) {
				result.link = _parent->rightActionLink();
			}
		}
	}
	return result;
}

bool GroupedMedia::toggleSelectionByHandlerClick(
		const ClickHandlerPtr &p) const {
	for (const auto &part : _parts) {
		if (part.content->toggleSelectionByHandlerClick(p)) {
			return true;
		}
	}
	return false;
}

bool GroupedMedia::dragItemByHandler(const ClickHandlerPtr &p) const {
	for (const auto &part : _parts) {
		if (part.content->dragItemByHandler(p)) {
			return true;
		}
	}
	return false;
}

TextSelection GroupedMedia::adjustSelection(
		TextSelection selection,
		TextSelectType type) const {
	return _caption.adjustSelection(selection, type);
}

TextForMimeData GroupedMedia::selectedText(
		TextSelection selection) const {
	return _caption.toTextForMimeData(selection);
}

auto GroupedMedia::getBubbleSelectionIntervals(
	TextSelection selection) const
-> std::vector<BubbleSelectionInterval> {
	auto result = std::vector<BubbleSelectionInterval>();
	for (auto i = 0, count = int(_parts.size()); i != count; ++i) {
		const auto &part = _parts[i];
		if (!IsGroupItemSelection(selection, i)) {
			continue;
		}
		const auto &geometry = part.geometry;
		if (result.empty()
			|| (result.back().top + result.back().height
				< geometry.top())
			|| (result.back().top > geometry.top() + geometry.height())) {
			result.push_back({ geometry.top(), geometry.height() });
		} else {
			auto &last = result.back();
			const auto newTop = std::min(last.top, geometry.top());
			const auto newHeight = std::max(
				last.top + last.height - newTop,
				geometry.top() + geometry.height() - newTop);
			last = BubbleSelectionInterval{ newTop, newHeight };
		}
	}
	if (IsGroupItemSelection(selection, _parts.size() - 1)) {
		result.back().height = height() - result.back().top;
	}
	return result;
}

void GroupedMedia::clickHandlerActiveChanged(
		const ClickHandlerPtr &p,
		bool active) {
	for (const auto &part : _parts) {
		part.content->clickHandlerActiveChanged(p, active);
	}
}

void GroupedMedia::clickHandlerPressedChanged(
		const ClickHandlerPtr &p,
		bool pressed) {
	for (const auto &part : _parts) {
		part.content->clickHandlerPressedChanged(p, pressed);
		if (pressed && part.content->dragItemByHandler(p)) {
			// #TODO drag by item from album
			// App::pressedLinkItem(part.view);
		}
	}
}

template <typename DataMediaRange>
bool GroupedMedia::applyGroup(const DataMediaRange &medias) {
	if (validateGroupParts(medias)) {
		return true;
	}

	auto modeChosen = false;
	for (const auto media : medias) {
		const auto mediaMode = DetectMode(media);
		if (!modeChosen) {
			_mode = mediaMode;
			modeChosen = true;
		} else if (mediaMode != _mode) {
			continue;
		}
		_parts.push_back(Part(_parent, media));
	}
	if (_parts.empty()) {
		return false;
	}

	Ensures(_parts.size() <= kMaxSize);
	return true;
}

template <typename DataMediaRange>
bool GroupedMedia::validateGroupParts(
		const DataMediaRange &medias) const {
	auto i = 0;
	const auto count = _parts.size();
	for (const auto media : medias) {
		if (i >= count || _parts[i].item != media->parent()) {
			return false;
		}
		++i;
	}
	return (i == count);
}

not_null<Media*> GroupedMedia::main() const {
	Expects(!_parts.empty());

	return _parts.back().content.get();
}

TextWithEntities GroupedMedia::getCaption() const {
	return main()->getCaption();
}

Storage::SharedMediaTypesMask GroupedMedia::sharedMediaTypes() const {
	return main()->sharedMediaTypes();
}

PhotoData *GroupedMedia::getPhoto() const {
	return main()->getPhoto();
}

DocumentData *GroupedMedia::getDocument() const {
	return main()->getDocument();
}

HistoryMessageEdited *GroupedMedia::displayedEditBadge() const {
	if (!_caption.isEmpty()) {
		return _parts.front().item->Get<HistoryMessageEdited>();
	}
	return nullptr;
}

void GroupedMedia::updateNeedBubbleState() {
	const auto captionItem = [&]() -> HistoryItem* {
		if (_mode == Mode::Column) {
			const auto last = _parts.back().item.get();
			return last->emptyText() ? nullptr : last;
		}
		auto result = (HistoryItem*)nullptr;
		for (const auto &part : _parts) {
			if (!part.item->emptyText()) {
				if (result) {
					return nullptr;
				} else {
					result = part.item;
				}
			}
		}
		return result;
	}();
	if (captionItem) {
		_caption = createCaption(captionItem);
	}
	_needBubble = computeNeedBubble();
}

void GroupedMedia::stopAnimation() {
	for (const auto &part : _parts) {
		part.content->stopAnimation();
	}
}

void GroupedMedia::checkAnimation() {
	for (const auto &part : _parts) {
		part.content->checkAnimation();
	}
}

bool GroupedMedia::hasHeavyPart() const {
	for (const auto &part : _parts) {
		if (part.content->hasHeavyPart()) {
			return true;
		}
	}
	return false;
}

void GroupedMedia::unloadHeavyPart() {
	for (const auto &part : _parts) {
		part.content->unloadHeavyPart();
	}
}

void GroupedMedia::parentTextUpdated() {
	history()->owner().requestViewResize(_parent);
}

bool GroupedMedia::needsBubble() const {
	return _needBubble;
}

bool GroupedMedia::computeNeedBubble() const {
	if (!_caption.isEmpty() || _mode == Mode::Column) {
		return true;
	}
	if (const auto item = _parent->data()) {
		if (item->repliesAreComments()
			|| item->externalReply()
			|| item->viaBot()
			|| _parent->displayedReply()
			|| _parent->displayForwardedFrom()
			|| _parent->displayFromName()
			) {
			return true;
		}
	}
	return false;
}

bool GroupedMedia::needInfoDisplay() const {
	return (_mode != Mode::Column)
		&& (_parent->data()->id < 0
			|| _parent->isUnderCursor()
			|| _parent->isLastAndSelfMessage());
}

} // namespace HistoryView
