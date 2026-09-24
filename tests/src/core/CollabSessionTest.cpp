/*
 * CollabSessionTest.cpp - end-to-end test: LMMS model <-> collaboration server <-> fake collaborator
 *
 * Copyright (c) 2026 adri21mg
 *
 * This file is part of LMMS - https://lmms.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program (see COPYING); if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 */

#include <QDomDocument>
#include <QJsonArray>
#include <QTextStream>
#include <QProcess>
#include <QRandomGenerator>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

#include "CollabProtocol.h"
#include "CollabSession.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "Note.h"
#include "Song.h"

using namespace lmms;
using namespace lmms::collab;

//! Minimal second collaborator speaking the wire protocol directly
class FakePeer
{
public:
	bool connectTo(quint16 port)
	{
		m_socket.connectToHost("127.0.0.1", port);
		if (!m_socket.waitForConnected(3000)) { return false; }
		send({{"t", "hello"}, {"proto", proto::Version}, {"user", "Yeray"}});
		const auto welcome = next("welcome");
		m_clientId = welcome.value("clientId").toString();
		return !m_clientId.isEmpty();
	}

	void send(const QJsonObject& m) { m_socket.write(proto::encodeJsonFrame(m)); m_socket.flush(); }

	void sendOps(const QJsonArray& ops) { send({{"t", "tx"}, {"ctx", ++m_ctx}, {"ops", ops}}); }

	//! Next message (optionally of a given type), waiting up to @p timeoutMs; empty object on timeout
	QJsonObject next(const QString& type = {}, int timeoutMs = 3000)
	{
		QElapsedTimer timer;
		timer.start();
		while (timer.elapsed() < timeoutMs)
		{
			proto::FrameType t;
			QByteArray payload;
			while (m_decoder.next(t, payload))
			{
				const auto msg = proto::parseMessage(payload);
				if (msg && (type.isEmpty() || msg->value("t").toString() == type)) { return *msg; }
			}
			// Keep the LMMS side (same thread) running while we wait for data
			QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
			if (m_socket.bytesAvailable() > 0 || m_socket.waitForReadyRead(5))
			{
				m_decoder.append(m_socket.readAll());
			}
		}
		return {};
	}

	//! Next transaction sent by someone other than us
	QJsonObject nextForeignTx(int timeoutMs = 3000)
	{
		QElapsedTimer timer;
		timer.start();
		while (timer.elapsed() < timeoutMs)
		{
			const auto tx = next("tx", timeoutMs - static_cast<int>(timer.elapsed()));
			if (tx.isEmpty()) { break; }
			if (tx.value("clientId").toString() != m_clientId) { return tx; }
		}
		return {};
	}

	//! Next operation of type @p opType sent by someone else (other ops, e.g. derived clip lengths, are skipped)
	QJsonObject nextForeignOp(const QString& opType, int timeoutMs = 3000)
	{
		QElapsedTimer timer;
		timer.start();
		while (timer.elapsed() < timeoutMs)
		{
			if (!m_ops.isEmpty())
			{
				const QJsonObject op = m_ops.takeFirst();
				if (op.value("op").toString() == opType) { return op; }
				continue;
			}
			const auto tx = nextForeignTx(timeoutMs - static_cast<int>(timer.elapsed()));
			if (tx.isEmpty()) { break; }
			for (const auto& op : tx.value("ops").toArray()) { m_ops.append(op.toObject()); }
		}
		return {};
	}

	//! Forgets everything received so far
	void drain()
	{
		m_ops.clear();
		while (!next({}, 200).isEmpty()) {}
	}

	QString clientId() const { return m_clientId; }

private:
	QTcpSocket m_socket;
	proto::FrameDecoder m_decoder;
	QString m_clientId;
	qint64 m_ctx = 0;
	QList<QJsonObject> m_ops;
};


class CollabSessionTest : public QObject
{
	Q_OBJECT
private:
	QTemporaryDir m_dataDir;
	QProcess m_server;
	quint16 m_port = 0;
	MidiClip* m_clip = nullptr;
	FakePeer m_peer;

	static QJsonObject noteOp(const char* op, MidiClip* clip, collab_id_t id, const QJsonObject& v = {})
	{
		QJsonObject o{{"op", op}, {"clip", proto::idString(clip->collabId())}, {"id", proto::idString(id)}};
		if (!v.isEmpty()) { o.insert("v", v); }
		return o;
	}

	//! Lets queued network traffic and throttle timers run
	static void settle(int ms = 300)
	{
		QElapsedTimer t;
		t.start();
		while (t.elapsed() < ms) { QCoreApplication::processEvents(QEventLoop::AllEvents, 10); }
	}

private slots:
	void initTestCase()
	{
		Engine::init(true);
		QVERIFY(m_dataDir.isValid());
		m_port = static_cast<quint16>(43000 + QRandomGenerator::global()->bounded(1000));
		m_server.setProcessChannelMode(QProcess::ForwardedChannels);
		m_server.start(COLLAB_SERVER_EXE, {"--port", QString::number(m_port), "--data", m_dataDir.path()});
		QVERIFY(m_server.waitForStarted(5000));
		QTest::qWait(500);

		auto track = dynamic_cast<InstrumentTrack*>(Track::create(Track::Type::Instrument, Engine::getSong()));
		m_clip = new MidiClip(track);
		m_clip->addNote(Note{TimePos{48}, TimePos{0}, 60}, false);
	}

	void cleanupTestCase()
	{
		CollabSession::instance()->disconnectFromServer();
		m_server.kill();
		m_server.waitForFinished(3000);
		Engine::destroy();
	}

	void testShareAndOpen()
	{
		auto session = CollabSession::instance();
		session->connectToServer("127.0.0.1", m_port, "Adri", "session-test", CollabSession::JoinMode::Create);
		QTRY_COMPARE_WITH_TIMEOUT(session->state(), CollabSession::State::Live, 5000);

		QVERIFY(m_peer.connectTo(m_port));
		m_peer.send({{"t", "open"}, {"project", "session-test"}});
		const auto joined = m_peer.next("joined");
		QDomDocument doc;
		QVERIFY(static_cast<bool>(doc.setContent(joined.value("mmp").toString())));
		const QString noteId = proto::idString(m_clip->notes().front()->collabId());
		bool found = false;
		const auto notes = doc.elementsByTagName("note");
		for (int i = 0; i < notes.size(); ++i)
		{
			found |= notes.at(i).toElement().attribute("cid") == noteId;
		}
		QVERIFY2(found, "the peer's snapshot contains our note with the same id");
	}

	void testLocalEditsReachPeer()
	{
		m_peer.drain();
		// add
		Note* note = m_clip->addNote(Note{TimePos{48}, TimePos{96}, 64}, false);
		auto op = m_peer.nextForeignOp("note.add");
		QCOMPARE(proto::parseId(op.value("id")), note->collabId());
		QCOMPARE(op.value("v").toObject().value("pos").toInt(), 96);
		QCOMPARE(op.value("v").toObject().value("key").toInt(), 64);

		// move (only changed fields are sent)
		note->setPos(TimePos{144});
		note->setKey(67);
		emit m_clip->dataChanged();
		op = m_peer.nextForeignOp("note.set");
		QCOMPARE(op.value("v").toObject(), (QJsonObject{{"pos", 144}, {"key", 67}}));

		// resize
		note->setLength(TimePos{96});
		emit m_clip->dataChanged();
		QCOMPARE(m_peer.nextForeignOp("note.set").value("v").toObject(), (QJsonObject{{"len", 96}}));

		// delete
		const collab_id_t id = note->collabId();
		m_clip->removeNote(note);
		QCOMPARE(proto::parseId(m_peer.nextForeignOp("note.remove").value("id")), id);
	}

	void testRemoteEditsApplyWithoutEcho()
	{
		m_peer.drain();
		const collab_id_t id = 0x00000000c0ffee01ULL;
		m_peer.sendOps({noteOp("note.add", m_clip, id,
			{{"key", 72}, {"pos", 48}, {"len", 24}, {"vol", 80}, {"pan", -10}, {"type", 0}})});
		QTRY_VERIFY(m_clip->findNote(id) != nullptr);
		Note* note = m_clip->findNote(id);
		QCOMPARE(note->key(), 72);
		QCOMPARE(note->getVolume(), 80);
		QCOMPARE(note->getPanning(), -10);

		m_peer.sendOps({noteOp("note.set", m_clip, id, {{"pos", 240}, {"len", 72}})});
		QTRY_COMPARE(m_clip->findNote(id)->pos().getTicks(), 240);
		QCOMPARE(m_clip->findNote(id)->length().getTicks(), 72);
		// notes stay sorted for playback
		QVERIFY(std::is_sorted(m_clip->notes().begin(), m_clip->notes().end(), Note::lessThan));

		m_peer.sendOps({noteOp("note.remove", m_clip, id)});
		QTRY_VERIFY(m_clip->findNote(id) == nullptr);

		// Applying remote changes must never send them back
		QVERIFY2(m_peer.nextForeignTx(500).isEmpty(), "no echo of remote changes");
	}

	void testConcurrentEditsConverge()
	{
		Note* note = m_clip->notes().front();
		const collab_id_t id = note->collabId();
		auto rng = QRandomGenerator::global();

		for (int round = 0; round < 40; ++round)
		{
			const int localKey = 20 + 2 * rng->bounded(40);  // even keys: ours
			const int remoteKey = 21 + 2 * rng->bounded(40); // odd keys: the peer's
			const bool localFirst = rng->bounded(2) == 0;
			const int gap = rng->bounded(80);

			auto local = [&] { m_clip->findNote(id)->setKey(localKey); emit m_clip->dataChanged(); };
			auto remote = [&] { m_peer.sendOps({noteOp("note.set", m_clip, id, {{"key", remoteKey}})}); };
			if (localFirst) { local(); settle(gap); remote(); }
			else { remote(); settle(gap); local(); }

			// The peer sees every transaction in server order; the last key it sees is the truth
			int truth = -1;
			QElapsedTimer quiet;
			quiet.start();
			while (quiet.elapsed() < 400)
			{
				const auto tx = m_peer.next("tx", 50);
				if (tx.isEmpty()) { continue; }
				for (const auto& o : tx.value("ops").toArray())
				{
					const auto v = o.toObject().value("v").toObject();
					if (v.contains("key")) { truth = v.value("key").toInt(); }
				}
				quiet.restart();
			}
			settle(100);
			QVERIFY2(truth != -1, "server ordered at least one edit");
			QCOMPARE(m_clip->findNote(id)->key(), truth);
		}
	}

private:
	//! Local edit gesture as an editor performs it: journal checkpoint first, then the change
	template<class F> void localGesture(F&& edit)
	{
		m_clip->addJournalCheckPoint();
		edit();
		emit m_clip->dataChanged();
		QVERIFY(!m_peer.nextForeignTx().isEmpty());
		m_peer.drain();
	}

	void remoteEdit(const QJsonObject& op)
	{
		m_peer.sendOps({op});
		settle(200);
		m_peer.drain();
	}

private slots:
	void testUndoOnlyRevertsOwnNotes()
	{
		auto journal = Engine::projectJournal();
		journal->setJournalling(true);

		collab_id_t mine = 0;
		localGesture([&] { mine = m_clip->addNote(Note{TimePos{48}, TimePos{384}, 50}, false)->collabId(); });
		const collab_id_t theirs = 0x00000000beef0001ULL;
		remoteEdit(noteOp("note.add", m_clip, theirs,
			{{"key", 52}, {"pos", 480}, {"len", 48}, {"vol", 100}, {"pan", 0}, {"type", 0}}));
		QVERIFY(m_clip->findNote(theirs));

		// Ctrl+Z removes my note but keeps the note Yeray added afterwards
		journal->undo();
		QVERIFY(m_clip->findNote(mine) == nullptr);
		QVERIFY(m_clip->findNote(theirs) != nullptr);
		QCOMPARE(proto::parseId(m_peer.nextForeignOp("note.remove").value("id")), mine);

		// Ctrl+Y brings it back with the same identity
		journal->redo();
		QVERIFY(m_clip->findNote(mine) != nullptr);
		QCOMPARE(m_clip->findNote(mine)->key(), 50);
		QCOMPARE(proto::parseId(m_peer.nextForeignOp("note.add").value("id")), mine);
		QVERIFY(m_clip->findNote(theirs) != nullptr);
	}

	void testUndoNeverOverwritesLaterChanges()
	{
		auto journal = Engine::projectJournal();
		const collab_id_t theirs = 0x00000000beef0001ULL;
		Note* note = m_clip->notes().front();
		const collab_id_t id = note->collabId();
		const int oldKey = note->key();
		const int oldPos = note->pos().getTicks();

		// I move the note, then Yeray moves it again: my Ctrl+Z must leave his change alone
		localGesture([&] { m_clip->findNote(id)->setKey(oldKey + 1); });
		remoteEdit(noteOp("note.set", m_clip, id, {{"key", oldKey + 5}}));
		journal->undo();
		QCOMPARE(m_clip->findNote(id)->key(), oldKey + 5);
		QVERIFY2(m_peer.nextForeignTx(400).isEmpty(), "nothing to revert, nothing sent");

		// Field by field: my position change is reverted, Yeray's volume change stays
		localGesture([&] { m_clip->findNote(id)->setPos(TimePos{oldPos + 48}); });
		remoteEdit(noteOp("note.set", m_clip, id, {{"vol", 42}}));
		journal->undo();
		QCOMPARE(m_clip->findNote(id)->pos().getTicks(), oldPos);
		QCOMPARE(m_clip->findNote(id)->getVolume(), 42);
		QCOMPARE(m_peer.nextForeignOp("note.set").value("v").toObject(), (QJsonObject{{"pos", oldPos}}));

		// Undoing a whole-track checkpoint never resurrects old notes or drops newer remote ones
		m_clip->getTrack()->addJournalCheckPoint();
		const collab_id_t later = 0x00000000beef0002ULL;
		remoteEdit(noteOp("note.add", m_clip, later,
			{{"key", 55}, {"pos", 600}, {"len", 48}, {"vol", 100}, {"pan", 0}, {"type", 0}}));
		remoteEdit(noteOp("note.remove", m_clip, theirs));
		journal->undo();
		settle(200);
		QVERIFY2(m_clip->findNote(later) != nullptr, "remote note added after the checkpoint is kept");
		QVERIFY2(m_clip->findNote(theirs) == nullptr, "remote removal after the checkpoint is kept");

		// ...and the clip is still shared
		m_peer.drain();
		m_clip->findNote(id)->setKey(33);
		emit m_clip->dataChanged();
		QCOMPARE(m_peer.nextForeignOp("note.set").value("v").toObject(), (QJsonObject{{"key", 33}}));
	}

	// ---- M2a: Song Editor structure ----

	void testLocalStructureReachesPeer()
	{
		m_peer.drain();
		auto song = Engine::getSong();

		// New track with a clip and a note: sent complete
		auto track = dynamic_cast<InstrumentTrack*>(Track::create(Track::Type::Instrument, song));
		auto clip = new MidiClip(track);
		clip->movePosition(TimePos{192});
		Note* note = clip->addNote(Note{TimePos{48}, TimePos{0}, 62}, false);
		auto op = m_peer.nextForeignOp("track.add");
		QDomDocument doc;
		QVERIFY(static_cast<bool>(doc.setContent(op.value("xml").toString())));
		QCOMPARE(proto::parseId(doc.documentElement().attribute("cid")), track->collabId());
		QVERIFY2(op.value("xml").toString().contains(proto::idString(clip->collabId())), "the clip travels with its track");
		QVERIFY2(op.value("xml").toString().contains(proto::idString(note->collabId())), "and its notes");
		m_newTrackXml = op.value("xml").toString();

		// Its notes are shared from now on
		note->setKey(63);
		emit clip->dataChanged();
		QCOMPARE(m_peer.nextForeignOp("note.set").value("v").toObject(), (QJsonObject{{"key", 63}}));

		// Clip move, rename, mute and color
		clip->movePosition(TimePos{384});
		QCOMPARE(m_peer.nextForeignOp("clip.set").value("v").toObject(), (QJsonObject{{"pos", 384}}));
		clip->setName("Riff");
		clip->toggleMute();
		clip->setColor(QColor{"#ff0000"});
		QCOMPARE(m_peer.nextForeignOp("clip.set").value("v").toObject(),
			(QJsonObject{{"name", "Riff"}, {"muted", true}, {"color", "#ff0000"}}));

		// Track rename and color
		track->setName("Lead");
		QCOMPARE(m_peer.nextForeignOp("track.set").value("v").toObject(), (QJsonObject{{"name", "Lead"}}));

		// New clip, then remove it
		auto second = new MidiClip(track);
		second->movePosition(TimePos{768});
		op = m_peer.nextForeignOp("clip.add");
		QCOMPARE(proto::parseId(op.value("track")), track->collabId());
		const collab_id_t secondId = second->collabId();
		delete second;
		QCOMPARE(proto::parseId(m_peer.nextForeignOp("clip.remove").value("id")), secondId);

		// Remove the track
		const collab_id_t trackId = track->collabId();
		delete track;
		QCOMPARE(proto::parseId(m_peer.nextForeignOp("track.remove").value("id")), trackId);
	}

	void testRemoteStructureApplies()
	{
		m_peer.drain();
		auto song = Engine::getSong();

		// A track created by the peer (reuse the XML format LMMS produced, with fresh ids)
		QString xml = m_newTrackXml;
		QDomDocument doc;
		QVERIFY(static_cast<bool>(doc.setContent(xml)));
		const collab_id_t trackId = 0x00000000aaaa0001ULL;
		const collab_id_t clipId = 0x00000000aaaa0002ULL;
		QDomElement root = doc.documentElement();
		root.setAttribute("cid", proto::idString(trackId));
		root.setAttribute("name", "From Yeray");
		root.firstChildElement("midiclip").setAttribute("cid", proto::idString(clipId));
		QString text;
		QTextStream ts{&text};
		root.save(ts, 0);
		m_peer.sendOps({QJsonObject{{"op", "track.add"}, {"container", "song"}, {"index", -1}, {"xml", text}}});
		QTRY_VERIFY(findTrack(trackId) != nullptr);
		QCOMPARE(findTrack(trackId)->name(), QString{"From Yeray"});
		QTRY_VERIFY(findClip(clipId) != nullptr);

		// Remote notes in the remote clip
		auto clip = dynamic_cast<MidiClip*>(findClip(clipId));
		const collab_id_t noteId = 0x00000000aaaa0003ULL;
		m_peer.sendOps({noteOp("note.add", clip, noteId,
			{{"key", 40}, {"pos", 0}, {"len", 48}, {"vol", 100}, {"pan", 0}, {"type", 0}})});
		QTRY_VERIFY(clip->findNote(noteId) != nullptr);

		// Clip changes, track rename
		m_peer.sendOps({QJsonObject{{"op", "clip.set"}, {"id", proto::idString(clipId)},
			{"v", QJsonObject{{"pos", 960}, {"name", "Bass"}}}}});
		QTRY_COMPARE(clip->startPosition().getTicks(), 960);
		QCOMPARE(clip->name(), QString{"Bass"});
		m_peer.sendOps({QJsonObject{{"op", "track.set"}, {"id", proto::idString(trackId)},
			{"v", QJsonObject{{"name", "Bass line"}}}}});
		QTRY_COMPARE(findTrack(trackId)->name(), QString{"Bass line"});

		// Order: move the remote track to the front of the shared tracks
		QJsonArray ids{proto::idString(trackId)};
		for (Track* t : song->tracks())
		{
			if (t->collabId() != trackId && t->type() != Track::Type::Pattern) { ids.append(proto::idString(t->collabId())); }
		}
		m_peer.sendOps({QJsonObject{{"op", "track.order"}, {"container", "song"}, {"ids", ids}}});
		QTRY_VERIFY(std::find_if(song->tracks().begin(), song->tracks().end(),
			[](Track* t) { return t->type() != Track::Type::Pattern; }) != song->tracks().end()
			&& (*std::find_if(song->tracks().begin(), song->tracks().end(),
			[](Track* t) { return t->type() != Track::Type::Pattern; }))->collabId() == trackId);

		QVERIFY2(m_peer.nextForeignTx(500).isEmpty(), "no echo of remote structure changes");

		// Remote removal of the clip and the track
		m_peer.sendOps({QJsonObject{{"op", "clip.remove"}, {"id", proto::idString(clipId)}}});
		QTRY_VERIFY(findClip(clipId) == nullptr);
		m_peer.sendOps({QJsonObject{{"op", "track.remove"}, {"id", proto::idString(trackId)}}});
		QTRY_VERIFY(findTrack(trackId) == nullptr);
		QVERIFY2(m_peer.nextForeignTx(500).isEmpty(), "no echo of remote removals");
	}

	void testSoloStaysPrivate()
	{
		m_peer.drain();
		auto song = Engine::getSong();
		auto a = Track::create(Track::Type::Instrument, song);
		auto b = Track::create(Track::Type::Instrument, song);
		m_peer.nextForeignOp("track.add");
		m_peer.nextForeignOp("track.add");
		m_peer.drain();

		// My solo mutes other tracks locally, but that must not reach the peer
		a->setSolo(true);
		a->toggleSolo(); // what the track's view does when its solo button changes
		QVERIFY(b->isMuted());
		QVERIFY2(m_peer.nextForeignOp("track.set", 500).isEmpty(), "solo is private");

		// The peer mutes b meanwhile: it becomes b's mute after my solo
		m_peer.sendOps({QJsonObject{{"op", "track.set"}, {"id", proto::idString(b->collabId())},
			{"v", QJsonObject{{"muted", true}}}}});
		settle(300);
		a->setSolo(false);
		a->toggleSolo();
		QVERIFY2(b->isMuted(), "the shared mute applies when my solo ends");
		QVERIFY2(!a->isMuted(), "a was never muted");
		QVERIFY2(m_peer.nextForeignOp("track.set", 500).isEmpty(), "ending my solo sends nothing");

		delete a;
		delete b;
		m_peer.drain();
	}

	void testClipUndoIsPerUser()
	{
		m_peer.drain();
		auto journal = Engine::projectJournal();
		Track* track = m_clip->getTrack();

		// I delete a clip (as the Song Editor does: track checkpoint, then removal)
		auto clip = new MidiClip(dynamic_cast<InstrumentTrack*>(track));
		clip->movePosition(TimePos{1536});
		const collab_id_t noteId = clip->addNote(Note{TimePos{48}, TimePos{0}, 70}, false)->collabId();
		m_peer.nextForeignOp("clip.add");
		m_peer.drain();
		const collab_id_t clipId = clip->collabId();
		track->addJournalCheckPoint();
		delete clip;
		QCOMPARE(proto::parseId(m_peer.nextForeignOp("clip.remove").value("id")), clipId);

		// Ctrl+Z brings it back with the same identity and its notes
		journal->undo();
		QVERIFY(findClip(clipId) != nullptr);
		QVERIFY(dynamic_cast<MidiClip*>(findClip(clipId))->findNote(noteId) != nullptr);
		const auto added = m_peer.nextForeignOp("clip.add");
		QVERIFY(added.value("xml").toString().contains(proto::idString(clipId)));

		// I move it, the peer moves it afterwards: my Ctrl+Z leaves the peer's position
		clip = dynamic_cast<MidiClip*>(findClip(clipId));
		clip->addJournalCheckPoint();
		clip->movePosition(TimePos{1920});
		m_peer.nextForeignOp("clip.set");
		m_peer.sendOps({QJsonObject{{"op", "clip.set"}, {"id", proto::idString(clipId)}, {"v", QJsonObject{{"pos", 2304}}}}});
		settle(300);
		m_peer.drain();
		journal->undo();
		QCOMPARE(clip->startPosition().getTicks(), 2304);

		// I add a clip, the peer adds a note into it: undoing my add must not delete the peer's work
		track->addJournalCheckPoint();
		auto mine = new MidiClip(dynamic_cast<InstrumentTrack*>(track));
		mine->movePosition(TimePos{3072});
		m_peer.nextForeignOp("clip.add");
		const collab_id_t mineId = mine->collabId();
		m_peer.sendOps({noteOp("note.add", mine, 0x00000000bbbb0001ULL,
			{{"key", 60}, {"pos", 0}, {"len", 48}, {"vol", 100}, {"pan", 0}, {"type", 0}})});
		settle(300);
		journal->undo();
		QVERIFY2(findClip(mineId) != nullptr, "a clip that others changed is not removed by my undo");
	}
private:
	QString m_newTrackXml;

	static Track* findTrack(collab_id_t id)
	{
		return const_cast<Track*>(static_cast<const Track*>(findOwner(IdScope::Track, id)));
	}
	static Clip* findClip(collab_id_t id)
	{
		return const_cast<Clip*>(static_cast<const Clip*>(findOwner(IdScope::Clip, id)));
	}
};

QTEST_GUILESS_MAIN(CollabSessionTest)
#include "CollabSessionTest.moc"
