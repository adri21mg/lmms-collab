/*
 * CollabIdTest.cpp - tests for persistent collaboration ids
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

#include <set>

#include <QDomDocument>
#include <QtTest>

#include "CollabId.h"
#include "DataFile.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "Note.h"
#include "Song.h"

class CollabIdTest : public QObject
{
	Q_OBJECT
private slots:
	void initTestCase()
	{
		lmms::Engine::init(true);
	}

	void cleanupTestCase()
	{
		lmms::Engine::destroy();
	}

	void testStringRoundTrip()
	{
		using namespace lmms;
		const collab_id_t id = 0x00a1b2c3d4e5f607ULL;
		QCOMPARE(collab::idToString(id), QString{"00a1b2c3d4e5f607"});
		QCOMPARE(collab::idFromString("00a1b2c3d4e5f607"), id);
		QCOMPARE(collab::idFromString("not-an-id"), collab_id_t{0});
		QCOMPARE(collab::idFromString(""), collab_id_t{0});
		QVERIFY(collab::newId() != 0);
	}

	void testRegistryGivesCopiesNewIds()
	{
		using namespace lmms::collab;
		int a = 0, b = 0;
		const auto idA = claimId(IdScope::Clip, 0, &a);
		QVERIFY(idA != 0);
		QCOMPARE(claimId(IdScope::Clip, idA, &a), idA); // same owner keeps it
		const auto idB = claimId(IdScope::Clip, idA, &b); // taken by a live object
		QVERIFY(idB != 0 && idB != idA);
		QCOMPARE(findOwner(IdScope::Clip, idA), static_cast<const void*>(&a));
		releaseId(IdScope::Clip, idA, &b); // not the owner: no effect
		QCOMPARE(findOwner(IdScope::Clip, idA), static_cast<const void*>(&a));
		releaseId(IdScope::Clip, idA, &a);
		releaseId(IdScope::Clip, idB, &b);
		QCOMPARE(findOwner(IdScope::Clip, idA), static_cast<const void*>(nullptr));
	}

	void testNotesGetUniqueIds()
	{
		using namespace lmms;
		auto track = dynamic_cast<InstrumentTrack*>(Track::create(Track::Type::Instrument, Engine::getSong()));
		QVERIFY(track);
		auto clip = new MidiClip(track);

		Note* n1 = clip->addNote(Note{TimePos{48}, TimePos{0}, 60}, false);
		QVERIFY(n1->collabId() != 0);
		// Adding a copy of an existing note (duplicate/paste) must produce a new identity
		Note* n2 = clip->addNote(*n1, false);
		QVERIFY(n2->collabId() != 0);
		QVERIFY(n2->collabId() != n1->collabId());
		QCOMPARE(clip->findNote(n1->collabId()), n1);
		QCOMPARE(clip->findNote(n2->collabId()), n2);

		delete track;
	}

	void testSaveLoadKeepsIdsAndRepairsDuplicates()
	{
		using namespace lmms;
		auto track = dynamic_cast<InstrumentTrack*>(Track::create(Track::Type::Instrument, Engine::getSong()));
		auto clip = new MidiClip(track);
		const collab_id_t id1 = clip->addNote(Note{TimePos{48}, TimePos{0}, 60}, false)->collabId();
		const collab_id_t id2 = clip->addNote(Note{TimePos{48}, TimePos{48}, 62}, false)->collabId();

		DataFile file{DataFile::Type::JournalData};
		clip->saveState(file, file.content());
		QDomElement saved = file.content().firstChildElement();
		QCOMPARE(collab::idFromString(saved.attribute(collab::IdAttribute)), clip->collabId());

		// Reloading the same clip (what undo does) keeps clip and note ids
		const collab_id_t clipId = clip->collabId();
		clip->restoreState(saved);
		QCOMPARE(clip->collabId(), clipId);
		QVERIFY(clip->findNote(id1) != nullptr);
		QVERIFY(clip->findNote(id2) != nullptr);

		// Loading the saved state into a second clip while the first is alive (paste) gives a new clip id
		auto copy = new MidiClip(track);
		copy->restoreState(saved);
		QVERIFY(copy->collabId() != clip->collabId());
		QCOMPARE(copy->notes().size(), std::size_t{2});

		// Duplicated note ids in a file are repaired on load
		QDomElement notes = saved.firstChildElement("note");
		notes.nextSiblingElement("note").setAttribute(collab::IdAttribute, notes.attribute(collab::IdAttribute));
		copy->restoreState(saved);
		std::set<collab_id_t> ids;
		for (const auto* n : copy->notes()) { ids.insert(n->collabId()); }
		QCOMPARE(ids.size(), std::size_t{2});
		QVERIFY(ids.count(0) == 0);

		delete track;
	}

	void testClonedTrackGetsNewIds()
	{
		using namespace lmms;
		auto track = Track::create(Track::Type::Instrument, Engine::getSong());
		auto clip = new MidiClip(dynamic_cast<InstrumentTrack*>(track));
		const collab_id_t clipId = clip->collabId();

		Track* cloned = track->clone();
		QVERIFY(cloned->collabId() != 0);
		QVERIFY(cloned->collabId() != track->collabId());
		QCOMPARE(cloned->getClips().size(), std::size_t{1});
		QVERIFY(cloned->getClips()[0]->collabId() != clipId);
		QCOMPARE(clip->collabId(), clipId);

		delete cloned;
		delete track;
	}
};

QTEST_GUILESS_MAIN(CollabIdTest)
#include "CollabIdTest.moc"
