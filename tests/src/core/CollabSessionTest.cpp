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

	QString clientId() const { return m_clientId; }

private:
	QTcpSocket m_socket;
	proto::FrameDecoder m_decoder;
	QString m_clientId;
	qint64 m_ctx = 0;
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
		// add
		Note* note = m_clip->addNote(Note{TimePos{48}, TimePos{96}, 64}, false);
		auto tx = m_peer.nextForeignTx();
		QCOMPARE(tx.value("ops").toArray().size(), 1);
		auto op = tx.value("ops").toArray().first().toObject();
		QCOMPARE(op.value("op").toString(), QString{"note.add"});
		QCOMPARE(proto::parseId(op.value("id")), note->collabId());
		QCOMPARE(op.value("v").toObject().value("pos").toInt(), 96);
		QCOMPARE(op.value("v").toObject().value("key").toInt(), 64);

		// move (only changed fields are sent)
		note->setPos(TimePos{144});
		note->setKey(67);
		emit m_clip->dataChanged();
		tx = m_peer.nextForeignTx();
		op = tx.value("ops").toArray().first().toObject();
		QCOMPARE(op.value("op").toString(), QString{"note.set"});
		QCOMPARE(op.value("v").toObject(), (QJsonObject{{"pos", 144}, {"key", 67}}));

		// resize
		note->setLength(TimePos{96});
		emit m_clip->dataChanged();
		tx = m_peer.nextForeignTx();
		QCOMPARE(tx.value("ops").toArray().first().toObject().value("v").toObject(), (QJsonObject{{"len", 96}}));

		// delete
		const collab_id_t id = note->collabId();
		m_clip->removeNote(note);
		tx = m_peer.nextForeignTx();
		op = tx.value("ops").toArray().first().toObject();
		QCOMPARE(op.value("op").toString(), QString{"note.remove"});
		QCOMPARE(proto::parseId(op.value("id")), id);
	}

	void testRemoteEditsApplyWithoutEcho()
	{
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
	}

	void remoteEdit(const QJsonObject& op)
	{
		m_peer.sendOps({op});
		settle(200);
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
		auto tx = m_peer.nextForeignTx();
		QCOMPARE(tx.value("ops").toArray().first().toObject().value("op").toString(), QString{"note.remove"});
		QCOMPARE(proto::parseId(tx.value("ops").toArray().first().toObject().value("id")), mine);

		// Ctrl+Y brings it back with the same identity
		journal->redo();
		QVERIFY(m_clip->findNote(mine) != nullptr);
		QCOMPARE(m_clip->findNote(mine)->key(), 50);
		tx = m_peer.nextForeignTx();
		QCOMPARE(tx.value("ops").toArray().first().toObject().value("op").toString(), QString{"note.add"});
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
		const auto tx = m_peer.nextForeignTx();
		QCOMPARE(tx.value("ops").toArray().first().toObject().value("v").toObject(), (QJsonObject{{"pos", oldPos}}));

		// Undoing a whole-track snapshot never resurrects old notes or drops newer remote ones
		m_clip->getTrack()->addJournalCheckPoint();
		const collab_id_t later = 0x00000000beef0002ULL;
		remoteEdit(noteOp("note.add", m_clip, later,
			{{"key", 55}, {"pos", 600}, {"len", 48}, {"vol", 100}, {"pan", 0}, {"type", 0}}));
		remoteEdit(noteOp("note.remove", m_clip, theirs));
		journal->undo(); // restores the track, which recreates its clips from the old snapshot
		settle(200);
		auto clip = dynamic_cast<MidiClip*>(m_clip->getTrack()->getClips().front());
		QVERIFY(clip != nullptr);
		m_clip = clip; // the clip object was recreated, with the same identity
		QVERIFY2(m_clip->findNote(later) != nullptr, "remote note added after the snapshot is kept");
		QVERIFY2(m_clip->findNote(theirs) == nullptr, "remote removal after the snapshot is kept");

		// ...and the recreated clip is still shared
		m_clip->findNote(id)->setKey(33);
		emit m_clip->dataChanged();
		QCOMPARE(m_peer.nextForeignTx().value("ops").toArray().first().toObject().value("v").toObject(),
			(QJsonObject{{"key", 33}}));
	}
};

QTEST_GUILESS_MAIN(CollabSessionTest)
#include "CollabSessionTest.moc"
