/**
 * \file composition.cpp
 * \brief This encapsulates a song composition. This includes the capability to
 *      parse a musicxml file and communicate the Notes (or some of the notes)
 *      to the Steam Controller.
 *
 * MIT License
 *
 * Copyright (c) 2019 Gregory Gluszek
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "composition.h"

#include <QFile>
#include <QMessageBox>
#include <QDebug>
#include <cmath>

/**
 * @brief Composition::Composition Constructor for class to help parse musicxml
 *      file and communicate Jingle Data to Steam Controller.
 *
 * @param filename musicxml filename containing music data.
 */
Composition::Composition(QString filename)
    : filename(filename)
    , parts(0)
    , currDivisions(1)
    , bpm(100)
    , currPart(0)
    , octaveAdjust(1.f)
    , partIdxR(0)
    , partIdxL(0)
    , measStartIdx(0)
    , measEndIdx(0)
{
}

/**
 * @brief Composition::parse Attempt to parse all the Note related data from the
 *      musicxml file specified via the constructor.
 *
 * @return Composition::ErrorCode
 */
Composition::ErrorCode Composition::parse() {
    QFile file(filename);

    QXmlStreamReader xml; //TODO: create on heap instead of stack maybe??

    parts.clear();
    currPart = 0;

    if (!file.open(QFile::ReadOnly | QFile::Text)) {
        qDebug() << "Failed to open file " << filename;
        return FILE_OPEN;
    }

    xml.setDevice(&file);

    while (xml.readNext() != QXmlStreamReader::EndDocument) {

        if (xml.tokenType() == QXmlStreamReader::StartElement) {
            if (xml.name() == QLatin1String("note")) {
                ErrorCode code = parseXmlNote(xml);
                if (code != NO_ERROR) {
                    qDebug() << "parseXmlNote() failed. Error: " << getErrorString(code);
                    return code;
                }
            } else if (xml.name() == QLatin1String("backup")) {
                ErrorCode code = parseXmlBackup(xml);
                if (code != NO_ERROR) {
                    qDebug() << "parseXmlBackup() failed. Error: " << getErrorString(code);
                    return code;
                }
            } else if (xml.name() == QLatin1String("measure")) {
                // Double check that all specified backups were seen through
                while(backups.size()) {
                    if (backups.top() != 0) {
                        qDebug() << "Reached beginning of measure with " << backups.size() << " backups "
                            "and top having value of " << backups.top();
                        return XML_PARSE;
                    }
                    backups.pop();
                    currPart = prevParts.top();
                    prevParts.pop();
                }

                // Add new measures for all parts below current
                for (uint32_t parts_idx = currPart; parts_idx < parts.size(); parts_idx++) {
                    Part& part = parts[parts_idx];

                    part.measures.push_back(Measure());
                }

            } else if (xml.name() == QLatin1String("per-minute")) {
                xml.readNext();
                bpm = xml.text().toUInt();
            } else if (xml.name() == QLatin1String("divisions")) {
                xml.readNext();
                currDivisions = xml.text().toUInt();
            }
        } else if (xml.tokenType() == QXmlStreamReader::EndElement) {
            if (xml.name() == QLatin1String("part")) {
                // Double check that all specified backups were seen through
                while(backups.size()) {
                    if (backups.top() != 0) {
                        qDebug() << "Reached end of part with " << backups.size() << " backups "
                            "and top having value of " << backups.top();
                        return XML_PARSE;
                    }
                    backups.pop();
                    currPart = prevParts.top();
                    prevParts.pop();
                }

                currPart++;
            }
        }
    }

    // TODO: final checks on parsed data?
    // TODO: make sure all parts have the same number of measures?

    // TODO: set default configurations for LEFT and RIGHT channels

    return NO_ERROR;
}

/**
 * @brief Composition::noteToCmd Convert information into command string to send to
 *      Controller via serial.
 *
 * @param[in] note Contains Note data.
 * @param chan Specificies which channel Note will be played on.
 * @param jingleIdx Defines which Jingle index the Jingle Data will exist under.
 * @param noteIdx Defines which Note in the Jingle we are programming.
 * @param chordIdx Defines which Note in chord to use.
 *
 * @return Command string to send to Controller via serial.
 */
QString Composition::noteToCmd(const Note& note, Channel chan, uint32_t jingleIdx,
        uint32_t noteIdx, uint32_t chordIdx) {

    QString chan_str("right");
    if (chan == LEFT) {
        chan_str = "left";
    }

    uint32_t duty_cydle = 128;
    uint32_t frequency = 0;
    if (chordIdx < note.frequencies.size()) {
        frequency = static_cast<uint32_t>(note.frequencies[chordIdx] * octaveAdjust);
    } else {
        qDebug() << "warning: chordIdx " << chordIdx << " out of range for note.frequencies.size() = " << note.frequencies.size();
    }


    uint32_t duration_ms = static_cast<uint32_t>(note.length * 60 * 1000 / bpm);

    QString cmd = QString("jingle note ") + QString::number(jingleIdx) + QString(" ") +
            chan_str + QString(" ") +
            QString::number(noteIdx) + QString(" ") +
            QString::number(duty_cydle) + QString(" ") +
            QString::number(frequency) + QString(" ") +
            QString::number(duration_ms) + QString("\n");

    return cmd;
}

/**
 * @brief Composition::download Download the Jingle data for each channel
 *      to the Controller via the providied serial port. This assumes that
 *      the musicxml has been successfully parsed and that the channels
 *      have been configured appropriately.
 *
 * @param serial Allows for communicating with Controller.
 * @param jingleIdx Defines which Jingle index the Jingle Data will exist under.
 *
 * @return Composition::ErrorCode
 */
Composition::ErrorCode Composition::download(SCSerial& serial, uint32_t jingleIdx) {
    // TODO: bounds check jingleIdx

    SCSerial::ErrorCode serial_err_code = SCSerial::NO_ERROR;
    QString cmd;
    QString resp;

//TODO: Also think through adding and jingleIdx, how do we make sure these line up...?

    // TODO: part selection and measure range should be based on configuration settings...
    const int parts_idx = 0;

    int num_notes = 0;

    if (parts_idx >= parts.size()) {
        qDebug() << "Not enough parts";
        return NO_ERROR;
    }

    Part& part = parts[parts_idx];

    for (uint32_t meas_idx = 0; meas_idx < part.measures.size(); meas_idx++) {
        Measure& meas = part.measures[meas_idx];
        num_notes += meas.notes.size();
    }

    cmd = "jingle add ";
    cmd += QString::number(num_notes) + QString(" ");
    cmd += QString::number(num_notes) + QString("\n");
    resp = cmd + "\rJingle added successfully.\n\r";
    serial_err_code = serial.send(cmd, resp);
    if (serial_err_code != SCSerial::NO_ERROR) {
        qDebug() << "serial.send() Error String: " << SCSerial::getErrorString(serial_err_code);
        return CMD_ERR;
    }

    uint32_t note_cnt = 0;

    for (uint32_t meas_idx = 0; meas_idx < part.measures.size(); meas_idx++) {
        Measure& meas = part.measures[meas_idx];
        for (uint32_t notes_idx = 0; notes_idx < meas.notes.size(); notes_idx++) {
            //TODO: note should be pulled based on channel...
            Note& note = meas.notes[notes_idx];

            cmd = noteToCmd(note, RIGHT, jingleIdx, note_cnt, 0);
            resp = cmd + "\rNote updated successfully.\n\r";
            serial_err_code = serial.send(cmd, resp);
            if (serial_err_code != SCSerial::NO_ERROR) {
                qDebug() << "serial.send() Error String: " << SCSerial::getErrorString(serial_err_code);
                return CMD_ERR;
            }
            cmd = noteToCmd(note, LEFT, jingleIdx, note_cnt, 0);
            resp = cmd + "\rNote updated successfully.\n\r";
            serial_err_code = serial.send(cmd, resp);
            if (serial_err_code != SCSerial::NO_ERROR) {
                qDebug() << "serial.send() Error String: " << SCSerial::getErrorString(serial_err_code);
                return CMD_ERR;
            }
            note_cnt++;
        }
    }

    return NO_ERROR;
}

/**
 * @brief Composition::parseXmlBackup Parse backup token from xml. This assumes xml is at
 *      desired backup token to be parsed.
 *
 * @return Composition::ErrorCode
 */
Composition::ErrorCode Composition::parseXmlBackup(QXmlStreamReader& xml) {
    uint32_t duration = 0;

    // Check that we are actually at the beginning of an XML backup token
    if (xml.name() != QLatin1String("backup") || xml.tokenType() != QXmlStreamReader::StartElement) {
        qDebug() << "XML is not at backup Start Element. XML Error String:" << xml.errorString();
        return XML_PARSE;
    }

    // Read all child tokens of backup token
    while(1) {
        xml.readNext();

        // Exit when all child tokens of backup token have been read
        if (xml.tokenType() == QXmlStreamReader::EndElement && xml.name() == QLatin1String("backup")) {
            break;
        }

        if (xml.tokenType() == QXmlStreamReader::StartElement) {
            if (xml.name() == QLatin1String("duration")) {
                xml.readNext();
                duration = xml.text().toUInt();
            }
        }
    }

    if (duration == 0) {
        qDebug() << "0 valued duration within backup token encountered";
        return XML_PARSE;
    }

    backups.push(duration);
    prevParts.push(currPart);
    currPart++;

    return NO_ERROR;
}

/**
 * @brief Composition::parseXmlNote Parse note token from xml. This assumes xml is at
 *      desired note token to be parsed.
 *
 * @return Composition::ErrorCode
 */
Composition::ErrorCode Composition::parseXmlNote(QXmlStreamReader& xml) {
    uint32_t raw_xml_duration = 0;
    float length = 0.f;
    float frequency = 0.f;
    bool isChord = false;

    // Check that we are actually at the beginning of an XML note token
    if (xml.name() != QLatin1String("note") || xml.tokenType() != QXmlStreamReader::StartElement) {
        qDebug() << "XML is not at note Start Element. XML Error String:" << xml.errorString();
        return XML_PARSE;
    }

    // Check if we should pop back up to part because we have covered duration we backed up via this note
    if (backups.size()) {
        if (backups.top() == 0) {
            backups.pop();
            currPart = prevParts.top();
            prevParts.pop();
        }
    }

    // Read all child tokens of note token
    while(1) {
        xml.readNext();

        // Exit when all child tokens of note token have been read
        if (xml.tokenType() == QXmlStreamReader::EndElement && xml.name() == QLatin1String("note")) {
            break;
        }

        if (xml.tokenType() == QXmlStreamReader::StartElement) {
            if (xml.name() == QLatin1String("pitch")) {
                ErrorCode code = parseXmlPitch(xml, frequency);
                if (code != NO_ERROR) {
                    qDebug() << "parsePitch() failed. Error: " << getErrorString(code);
                    return code;
                }
            } else if (xml.name() == QLatin1String("duration")) {
                xml.readNext();
                length = static_cast<float>(xml.text().toUInt()) / currDivisions;
                raw_xml_duration = xml.text().toUInt();
            } else if (xml.name() == QLatin1String("chord")) {
                isChord = true;
            }
        }

    }

    if (currPart >= parts.size()) {
        parts.push_back(Part());
    }

    Part& part = parts[currPart];

    if (part.measures.size() < 1) {
        part.measures.push_back(Measure());
    }

    Measure& meas = part.measures.back();

    if (isChord) {
        if (meas.notes.size() < 1) {
            qDebug() << "Received chord, but no note exists for the current measure...";
            return XML_PARSE;
        }

        Note& note = meas.notes.back();

        if (static_cast<int>(note.length) != static_cast<int>(length)) {
            qDebug() << "Warning: Length not consistent across notes in chord";
        }

        note.frequencies.push_back(frequency);
    } else {
        Note note;
        note.frequencies.push_back(frequency);
        note.length = static_cast<float>(length);
        meas.notes.push_back(note);
        meas.xmlDurationSum += raw_xml_duration;

        if (backups.size()) {
            uint32_t backup_dur = backups.top();
            if (raw_xml_duration > backup_dur) {
                qDebug() << "Remaining backup duration (" << backup_dur << ") is less than"
                    " current Note duration (" << raw_xml_duration << ")";
                return XML_PARSE;
            }

            // Update backup duration counter
            backups.pop();
            backups.push(backup_dur - raw_xml_duration);
        }
    }

    return NO_ERROR;
}

/**
 * @brief Composition::parseXmlPitch Parse pitch token from xml. This assumes xml is at
 *      desired pitch token to be parsed.
 *
 * @param[out] freq Frequency in Hz calculated from musicxml data.
 *
 * @return Composition::ErrorCode
 */
Composition::ErrorCode Composition::parseXmlPitch(QXmlStreamReader& xml, float& freq) {
    QChar step = 0;
    int alter = 0;
    int octave = 0;

    if (xml.name() != QLatin1String("pitch") || xml.tokenType() != QXmlStreamReader::StartElement) {
        qDebug() << "XML is not at pitch Start Element. XML Error String:" << xml.errorString();
        return XML_PARSE;
    }

    // Read all child tokens of pitch token
    while(1) {
        xml.readNext();

        // Exit when all child tokens of pitch token have been read
        if (xml.tokenType() == QXmlStreamReader::EndElement && xml.name() == QLatin1String("pitch")) {
            break;
        }

        if (xml.tokenType() == QXmlStreamReader::StartElement) {
            if (xml.name() == QLatin1String("step")) {
                xml.readNext();
                step = xml.text()[0];
            } else if (xml.name() == QLatin1String("alter")) {
                xml.readNext();
                alter = xml.text().toInt();
            } else if (xml.name() == QLatin1String("octave")) {
                xml.readNext();
                octave = xml.text().toInt();
            }
        }
    }

    // See http://pages.mtu.edu/~suits/NoteFreqCalcs.html for details on converting notes to frequencies
    const int HALF_STEPS_PER_OCTAVE = 12;

    int num_half_steps = octave * HALF_STEPS_PER_OCTAVE + alter;

    if (step == 'C') {
        num_half_steps += 0;
    } else if (step == 'D') {
        num_half_steps += 2;
    } else if (step == 'E') {
        num_half_steps += 4;
    } else if (step == 'F') {
        num_half_steps += 5;
    } else if (step == 'G') {
        num_half_steps += 7;
    } else if (step == 'A') {
        num_half_steps += 9;
    } else if (step == 'B') {
        num_half_steps += 11;
    } else {
        qDebug() << "Invalid step specification of " << step << " in XML.";
        return XML_PARSE;
    }

    const double twelth_root_of_two = 1.059463094359;
    double factor = 1;
    for (int cnt = 0; cnt < num_half_steps; cnt++) {
        factor *=  twelth_root_of_two;
    }

    const double C_0_FREQ = 16.35;
    freq = static_cast<float>(C_0_FREQ * factor);

    return NO_ERROR;
}

/**
 * @brief Composition::getMemUsage Function for calculating how the much EEPROM
 *      memory the Jingle data from this composition will take up. This is
 *      varies based on configuration and is used to make sure we do not try to
 *      write too much, or invalid, Jingle Data to the EEPROM.
 *
 * @return The number of bytes required to store the Jingle data, as currently
 *      configured, in EEPROM of the Controller.
 */
uint32_t Composition::getMemUsage() {
    static const uint32_t NUM_JINGLE_HDR_BYTES = 4;// Number of bytes required for each
            // Jingle to give data on Jingle (i.e. number of Notes per channel)
    static const uint32_t BYTES_PER_NOTE = 6; // Number of bytes required to store
        // a single Note in EEPROM

    uint32_t byte_cnt = NUM_JINGLE_HDR_BYTES;

    uint32_t part_idx_r = getPartIdx(RIGHT);
    uint32_t part_idx_l = getPartIdx(LEFT);
    uint32_t meas_start_idx = getMeasStartIdx();
    uint32_t meas_end_idx = getMeasEndIdx();
    const Part& part_r = parts[part_idx_r];
    const Part& part_l = parts[part_idx_l];
    for (uint32_t meas_idx = meas_start_idx; meas_idx < meas_end_idx; meas_idx++) {
        const Measure& meas_r = part_r.measures[meas_idx];
        byte_cnt += meas_r.notes.size() * BYTES_PER_NOTE;
        const Measure& meas_l = part_l.measures[meas_idx];
        byte_cnt += meas_l.notes.size() * BYTES_PER_NOTE;
    }

    return 0;
}

/**
 * @brief getNumMeasures Returns the number of measures in each of the
 *      parts. This allows a user to know how they can trim the Jingle data.
 *
 * @return The number of measure parsed from the musicxml file.
 */
uint32_t Composition::getNumMeasures() {
    if (!parts.size())
        return 0;

    return static_cast<uint32_t>(parts[0].measures.size());
}

/**
 * @brief getNumChords Given a range for a particular part this function
 *      checks for the largest chord. This is done as a user may want both
 *      channels to use the same part, but different notes from chords that
 *      might be withing that range.
 *
 * @param partIdx Defines which Part to consider.
 * @param measStartIdx Defines range of Measures to consider.
 * @param measEndIdx Defines range of Measures to consider.
 *
 * @return The size of the largest chord within the specified drange.
 */
uint32_t Composition::getNumChords(uint32_t partIdx, uint32_t measStartIdx, uint32_t measEndIdx) {
    if (partIdx >= parts.size()) {
        qDebug() << "Invalid partIdx of " << partIdx << " specified in CompositiongetNumChords";
        return 0;
    }

    const Part& part = parts[partIdx];

    if (measStartIdx >= part.measures.size() || measEndIdx >= part.measures.size()) {
        qDebug() << "Invalid range of " << measStartIdx << " to " << measEndIdx <<
            " specified in CompositiongetNumChords";
        return 0;
    }

    uint32_t max_chord_size = 0;
    for (uint32_t meas_idx = measStartIdx; meas_idx < measEndIdx; meas_idx++) {
        const Measure& meas = part.measures[meas_idx];
        for (uint32_t note_idx = 0; note_idx < meas.notes.size(); note_idx++) {
            const Note& note = meas.notes[note_idx];
            if (note.frequencies.size() > max_chord_size) {
                max_chord_size = static_cast<uint32_t>(note.frequencies.size());
            }
        }
    }

    return max_chord_size;
}

/**
 * @brief Composition::setPartIdx Configure a channel regarding which part
 *      it gets its Jingle Data from.
 *
 * @param chan Defines which haptic/output channel is being configured.
 * @param partIdx Defines which part to use for specified channel.
 *
 * @return NO_ERROR on success;
 */
Composition::ErrorCode Composition::setPartIdx(Channel chan, uint32_t partIdx) {
    switch (chan) {
    case RIGHT:
        if (partIdx >= parts.size()) {
            qDebug() << "Bad partIdx " << partIdx << " specified for Right Channel";
            return BAD_IDX;
        }
        partIdxR = partIdx;
        break;

    case LEFT:
        if (partIdx >= parts.size()) {
            qDebug() << "Bad partIdx " << partIdx << " specified for Left Channel";
            return BAD_IDX;
        }
        partIdxL = partIdx;
        break;
    }

    return NO_ERROR;
}

/**
 * @brief Composition::getPartIdx
 *
 * @param chan Defines which haptic/output channel is being referred to.
 *
 * @return Index relating with Part specified channel is pulling notes from.
 */
uint32_t Composition::getPartIdx(Channel chan) {
    switch (chan) {
    case RIGHT:
        return partIdxR;

    case LEFT:
        return partIdxL;
    }
}

/**
 * @brief Composition::setMeasStartIdx Allows for trimming where Jingle Data
 *      starts in parsed data.
 *
 * @param measStartIdx Defines start point of Jingle Data.
 *
 * @return NO_ERROR on success.
 */
Composition::ErrorCode Composition::setMeasStartIdx(uint32_t measStartIdx) {
    if (!parts.size()) {
        qDebug() << "Cannot setMeasStartIdx if there are no parts";
        return BAD_IDX;
    }

    const Part& part = parts[0];

    if (measStartIdx >= part.measures.size()) {
        qDebug() << "Invalid range measStartIdx " << measStartIdx << " specified";
        return BAD_IDX;
    }

    this->measStartIdx = measStartIdx;

    return NO_ERROR;
}

/**
 * @brief Composition::getMeasStartIdx
 *
 * @return Where Jingle data is configured to start in parsed data.
 */
uint32_t Composition::getMeasStartIdx() {
    return measStartIdx;
}

/**
 * @brief Composition::setMeasEndIdx Allows for trimming where Jingle Data
 *      ends in parsed data.
 *
 * @param measStartIdx Defines start point of Jingle Data.
 *
 * @return NO_ERROR on success.
 */
Composition::ErrorCode Composition::setMeasEndIdx(uint32_t measEndIdx) {
    if (!parts.size()) {
        qDebug() << "Cannot setMeasEndIdx if there are no parts";
        return BAD_IDX;
    }

    const Part& part = parts[0];

    if (measStartIdx >= part.measures.size()) {
        qDebug() << "Invalid range measStartIdx " << measStartIdx << " specified";
        return BAD_IDX;
    }

    this->measEndIdx = measEndIdx;

    return NO_ERROR;
}

/**
 * @brief Composition::getMeasEndIdx
 *
 * @return Where Jingle data is configured to endin parsed data.
 */
uint32_t Composition::getMeasEndIdx() {
    return measEndIdx;
}
