#include <QDataStream>
#include <QPainter>
#include <QPixmapCache>
#include <QtConcurrent>
#include "osm.h"
#include "tile.h"
#include "gemfmap.h"

static bool readSources(QDataStream &stream)
{
	qint32 num, len, idx;
	QByteArray name;

	stream >> num;

	for (int i = 0; i < num; i++) {
		stream >> idx >> len;
		name.resize(len);
		stream.readRawData(name.data(), name.size());
	}

	return (stream.status() == QDataStream::Ok);
}

QRect GEMFMap::rect(const Zoom &zoom) const
{
	QRect r;
	const QList<Region> &list = zoom.ranges;

	for (int i = 0; i < list.size(); i++) {
		const Region &rg = list.at(i);
		r |= QRect(QPoint(rg.minX, rg.minY), QPoint(rg.maxX, rg.maxY));
	}

	return r;
}

bool GEMFMap::readHeader(QDataStream &stream)
{
	qint32 version;

	stream >> version >> _tileSize;
	return (stream.status() == QDataStream::Ok
	  && (version >= 3 && version <= 4));
}

bool GEMFMap::readRegions(QDataStream &stream)
{
	qint32 num, idx, level;
	Region r;
	int zi;

	stream >> num;

	for (int i = 0; i < num; i++) {
		stream >> level >> r.minX >> r.maxX >> r.minY >> r.maxY >> idx
		  >> r.offset;

		if ((zi = _zooms.indexOf(level)) < 0) {
			zi = _zooms.size();
			_zooms.append(Zoom(level));
		}

		_zooms[zi].ranges.append(r);
	}

	return (stream.status() == QDataStream::Ok);
}

bool GEMFMap::computeBounds()
{
	if (_zooms.isEmpty())
		return false;

	std::sort(_zooms.begin(), _zooms.end());

	const Zoom &z = _zooms.first();
	QRect r(rect(z));
	if (!r.isValid())
		return false;

	Coordinates tl(OSM::tile2ll(r.topLeft(), z.level));
	tl.rlat() = -tl.lat();
	Coordinates br(OSM::tile2ll(QPoint(r.right() + 1, r.bottom() + 1), z.level));
	br.rlat() = -br.lat();
	// Workaround of broken zoom levels 0 and 1 due to numerical
	// instability
	tl.rlat() = qMin(tl.lat(), OSM::BOUNDS.top());
	br.rlat() = qMax(br.lat(), OSM::BOUNDS.bottom());

	_bounds = RectC(tl, br);

	return true;
}

GEMFMap::GEMFMap(const QString &fileName, QObject *parent)
  : Map(fileName, parent), _file(fileName), _zi(0), _mapRatio(1.0),
  _valid(false)
{
	if (!_file.open(QIODevice::ReadOnly)) {
		_errorString = _file.errorString();
		return;
	}

	QDataStream stream(&_file);

	if (!readHeader(stream)) {
		_errorString = "Invalid/unsupported GEMF file";
		return;
	}
	if (!readSources(stream)) {
		_errorString = "Error reading tile sources";
		return;
	}
	if (!readRegions(stream)) {
		_errorString = "Error reading tile ranges";
		return;
	}
	if (!computeBounds()) {
		_errorString = "Invalid map area";
		return;
	}

	_file.close();

	_valid = true;
}

qreal GEMFMap::resolution(const QRectF &rect)
{
	return OSM::resolution(rect.center(), _zooms.at(_zi).level, _tileSize);
}

int GEMFMap::zoomFit(const QSize &size, const RectC &rect)
{
	if (!rect.isValid())
		_zi = _zooms.size() - 1;
	else {
		QRectF tbr(OSM::ll2m(rect.topLeft()), OSM::ll2m(rect.bottomRight()));
		QPointF sc(tbr.width() / size.width(), tbr.height() / size.height());
		int zoom = OSM::scale2zoom(qMax(sc.x(), -sc.y()) / _mapRatio,
		  _tileSize);

		_zi = 0;
		for (int i = 1; i < _zooms.size(); i++) {
			if (_zooms.at(i).level > zoom)
				break;
			_zi = i;
		}
	}

	return _zi;
}

int GEMFMap::zoomIn()
{
	_zi = qMin(_zi + 1, _zooms.size() - 1);
	return _zi;
}

int GEMFMap::zoomOut()
{
	_zi = qMax(_zi - 1, 0);
	return _zi;
}

QRectF GEMFMap::bounds()
{
	return QRectF(ll2xy(_bounds.topLeft()), ll2xy(_bounds.bottomRight()));
}

QPointF GEMFMap::ll2xy(const Coordinates &c)
{
	qreal scale = OSM::zoom2scale(_zooms.at(_zi).level, _tileSize);
	QPointF m = OSM::ll2m(c);
	return QPointF(m.x() / scale, m.y() / -scale) / _mapRatio;
}

Coordinates GEMFMap::xy2ll(const QPointF &p)
{
	qreal scale = OSM::zoom2scale(_zooms.at(_zi).level, _tileSize);
	return OSM::m2ll(QPointF(p.x() * scale, -p.y() * scale) * _mapRatio);
}

void GEMFMap::load()
{
	_file.open(QIODevice::ReadOnly);
}

void GEMFMap::unload()
{
	_file.close();
}

qreal GEMFMap::tileSize() const
{
	return (_tileSize / _mapRatio);
}

QByteArray GEMFMap::tileData(const QPoint &tile)
{
	const Zoom &z = _zooms.at(_zi);

	for(int i = 0; i < z.ranges.size(); i++) {
		const Region &r = z.ranges.at(i);
		QRect rect(QPoint(r.minX, r.minY), QPoint(r.maxX, r.maxY));

		if (rect.contains(tile)) {
			quint32 x = tile.x() - r.minX;
			quint32 y = tile.y() - r.minY;
			quint32 idx = x * (r.maxY + 1 - r.minY) + y;
			quint64 offset = idx * 12;
			quint64 address;
			quint32 size;

			if (!_file.seek(offset + r.offset))
				return QByteArray();
			QDataStream stream(&_file);
			stream >> address >> size;
			if (stream.status() != QDataStream::Ok)
				return QByteArray();

			if (!_file.seek(address))
				return QByteArray();

			QByteArray data;
			data.resize(size);
			return (_file.read(data.data(), data.size()) == size)
			  ? data : QByteArray();
		}
	}

	return QByteArray();
}

void GEMFMap::draw(QPainter *painter, const QRectF &rect, Flags flags)
{
	Q_UNUSED(flags);
	const Zoom &z = _zooms.at(_zi);
	qreal scale = OSM::zoom2scale(z.level, _tileSize);
	QRectF b(bounds());


	QPoint tile = OSM::mercator2tile(QPointF(rect.topLeft().x() * scale,
	  -rect.topLeft().y() * scale) * _mapRatio, z.level);
	QPointF tl(floor(rect.left() / tileSize())
	  * tileSize(), floor(rect.top() / tileSize()) * tileSize());

	QSizeF s(qMin(rect.right() - tl.x(), b.width()),
	  qMin(rect.bottom() - tl.y(), b.height()));
	int width = ceil(s.width() / tileSize());
	int height = ceil(s.height() / tileSize());


	QList<RenderTile> tiles;

	for (int i = 0; i < width; i++) {
		for (int j = 0; j < height; j++) {
			QPixmap pm;
			QPoint t(tile.x() + i, tile.y() + j);
			QString key = path() + "-" + QString::number(z.level) + "_"
			  + QString::number(t.x()) + "_" + QString::number(t.y());

			if (QPixmapCache::find(key, &pm)) {
				QPointF tp(qMax(tl.x(), b.left()) + (t.x() - tile.x())
				  * tileSize(), qMax(tl.y(), b.top()) + (t.y() - tile.y())
				  * tileSize());
				drawTile(painter, pm, tp);
			} else {
				tiles.append(RenderTile(t, tileData(t), key));
			}
		}
	}

	QFuture<void> future = QtConcurrent::map(tiles, &RenderTile::load);
	future.waitForFinished();

	for (int i = 0; i < tiles.size(); i++) {
		const RenderTile &mt = tiles.at(i);
		QPixmap pm(mt.pixmap());
		if (pm.isNull())
			continue;

		QPixmapCache::insert(mt.key(), pm);

		QPointF tp(qMax(tl.x(), b.left()) + (mt.xy().x() - tile.x())
		  * tileSize(), qMax(tl.y(), b.top()) + (mt.xy().y() - tile.y())
		  * tileSize());
		drawTile(painter, pm, tp);
	}
}

void GEMFMap::drawTile(QPainter *painter, QPixmap &pixmap, QPointF &tp)
{
	pixmap.setDevicePixelRatio(_mapRatio);
	painter->drawPixmap(tp, pixmap);
}

Map *GEMFMap::create(const QString &path, const Projection &, bool *isDir)
{
	if (isDir)
		*isDir = false;

	return new GEMFMap(path);
}

#ifndef QT_NO_DEBUG
QDebug operator<<(QDebug dbg, const GEMFMap::Region &region)
{
	dbg.nospace() << "Region(" << QRect(QPoint(region.minX, region.minY),
	  QPoint(region.maxX, region.maxY)) << ", " << region.offset << ")";

	return dbg.space();
}

QDebug operator<<(QDebug dbg, const GEMFMap::Zoom &zoom)
{
	dbg.nospace() << "Zoom(" << zoom.level << ", " << zoom.ranges << ")";

	return dbg.space();
}
#endif // QT_NO_DEBUG
