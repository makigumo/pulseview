/*
 * This file is part of the PulseView project.
 *
 * Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <libsigrokdecode/libsigrokdecode.h>

#include <stdexcept>

#include <QDebug>

#include "decoderstack.hpp"

#include <pv/data/decode/annotation.hpp>
#include <pv/data/decode/decoder.hpp>
#include <pv/data/logic.hpp>
#include <pv/data/logicsegment.hpp>
#include <pv/session.hpp>
#include <pv/views/trace/logicsignal.hpp>

using std::lock_guard;
using std::mutex;
using std::unique_lock;
using std::deque;
using std::make_pair;
using std::max;
using std::min;
using std::list;
using std::shared_ptr;
using std::make_shared;
using std::vector;

using boost::optional;

using namespace pv::data::decode;

namespace pv {
namespace data {

const double DecoderStack::DecodeMargin = 1.0;
const double DecoderStack::DecodeThreshold = 0.2;
const int64_t DecoderStack::DecodeChunkLength = 10 * 1024 * 1024;
const unsigned int DecoderStack::DecodeNotifyPeriod = 1024;

mutex DecoderStack::global_srd_mutex_;

DecoderStack::DecoderStack(pv::Session &session,
	const srd_decoder *const dec) :
	session_(session),
	start_time_(0),
	samplerate_(0),
	sample_count_(0),
	frame_complete_(false),
	samples_decoded_(0)
{
	connect(&session_, SIGNAL(frame_began()),
		this, SLOT(on_new_frame()));
	connect(&session_, SIGNAL(data_received()),
		this, SLOT(on_data_received()));
	connect(&session_, SIGNAL(frame_ended()),
		this, SLOT(on_frame_ended()));

	stack_.push_back(make_shared<decode::Decoder>(dec));
}

DecoderStack::~DecoderStack()
{
	if (decode_thread_.joinable()) {
		interrupt_ = true;
		input_cond_.notify_one();
		decode_thread_.join();
	}
}

const list< shared_ptr<decode::Decoder> >& DecoderStack::stack() const
{
	return stack_;
}

void DecoderStack::push(shared_ptr<decode::Decoder> decoder)
{
	assert(decoder);
	stack_.push_back(decoder);
}

void DecoderStack::remove(int index)
{
	assert(index >= 0);
	assert(index < (int)stack_.size());

	// Find the decoder in the stack
	auto iter = stack_.begin();
	for (int i = 0; i < index; i++, iter++)
		assert(iter != stack_.end());

	// Delete the element
	stack_.erase(iter);
}

double DecoderStack::samplerate() const
{
	return samplerate_;
}

const pv::util::Timestamp& DecoderStack::start_time() const
{
	return start_time_;
}

int64_t DecoderStack::samples_decoded() const
{
	lock_guard<mutex> decode_lock(output_mutex_);
	return samples_decoded_;
}

vector<Row> DecoderStack::get_visible_rows() const
{
	lock_guard<mutex> lock(output_mutex_);

	vector<Row> rows;

	for (const shared_ptr<decode::Decoder> &dec : stack_) {
		assert(dec);
		if (!dec->shown())
			continue;

		const srd_decoder *const decc = dec->decoder();
		assert(dec->decoder());

		// Add a row for the decoder if it doesn't have a row list
		if (!decc->annotation_rows)
			rows.emplace_back(decc);

		// Add the decoder rows
		for (const GSList *l = decc->annotation_rows; l; l = l->next) {
			const srd_decoder_annotation_row *const ann_row =
				(srd_decoder_annotation_row *)l->data;
			assert(ann_row);
			rows.emplace_back(decc, ann_row);
		}
	}

	return rows;
}

uint64_t DecoderStack::inc_annotation_count()
{
	return (annotation_count_++);
}

void DecoderStack::get_annotation_subset(
	vector<pv::data::decode::Annotation> &dest,
	const Row &row, uint64_t start_sample,
	uint64_t end_sample) const
{
	lock_guard<mutex> lock(output_mutex_);

	const auto iter = rows_.find(row);
	if (iter != rows_.end())
		(*iter).second.get_annotation_subset(dest,
			start_sample, end_sample);
}

QString DecoderStack::error_message()
{
	lock_guard<mutex> lock(output_mutex_);
	return error_message_;
}

void DecoderStack::clear()
{
	sample_count_ = 0;
	annotation_count_ = 0;
	frame_complete_ = false;
	samples_decoded_ = 0;
	error_message_ = QString();
	rows_.clear();
	class_rows_.clear();
}

void DecoderStack::begin_decode()
{
	if (decode_thread_.joinable()) {
		interrupt_ = true;
		input_cond_.notify_one();
		decode_thread_.join();
	}

	clear();

	// Check that all decoders have the required channels
	for (const shared_ptr<decode::Decoder> &dec : stack_)
		if (!dec->have_required_channels()) {
			error_message_ = tr("One or more required channels "
				"have not been specified");
			return;
		}

	// Add classes
	for (const shared_ptr<decode::Decoder> &dec : stack_) {
		assert(dec);
		const srd_decoder *const decc = dec->decoder();
		assert(dec->decoder());

		// Add a row for the decoder if it doesn't have a row list
		if (!decc->annotation_rows)
			rows_[Row(decc)] = decode::RowData();

		// Add the decoder rows
		for (const GSList *l = decc->annotation_rows; l; l = l->next) {
			const srd_decoder_annotation_row *const ann_row =
				(srd_decoder_annotation_row *)l->data;
			assert(ann_row);

			const Row row(decc, ann_row);

			// Add a new empty row data object
			rows_[row] = decode::RowData();

			// Map out all the classes
			for (const GSList *ll = ann_row->ann_classes;
				ll; ll = ll->next)
				class_rows_[make_pair(decc,
					GPOINTER_TO_INT(ll->data))] = row;
		}
	}

	// We get the logic data of the first channel in the list.
	// This works because we are currently assuming all
	// logic signals have the same data/segment
	pv::data::SignalBase *signalbase;
	pv::data::Logic *data = nullptr;

	for (const shared_ptr<decode::Decoder> &dec : stack_)
		if (dec && !dec->channels().empty() &&
			((signalbase = (*dec->channels().begin()).second.get())) &&
			((data = signalbase->logic_data().get())))
			break;

	if (!data)
		return;

	// Check we have a segment of data
	const deque< shared_ptr<pv::data::LogicSegment> > &segments =
		data->logic_segments();
	if (segments.empty())
		return;
	segment_ = segments.front();

	// Get the samplerate and start time
	start_time_ = segment_->start_time();
	samplerate_ = segment_->samplerate();
	if (samplerate_ == 0.0)
		samplerate_ = 1.0;

	interrupt_ = false;
	decode_thread_ = std::thread(&DecoderStack::decode_proc, this);
}

uint64_t DecoderStack::max_sample_count() const
{
	uint64_t max_sample_count = 0;

	for (const auto& row : rows_)
		max_sample_count = max(max_sample_count,
			row.second.get_max_sample());

	return max_sample_count;
}

optional<int64_t> DecoderStack::wait_for_data() const
{
	unique_lock<mutex> input_lock(input_mutex_);

	// Do wait if we decoded all samples but we're still capturing
	// Do not wait if we're done capturing
	while (!interrupt_ && !frame_complete_ &&
		(samples_decoded_ >= sample_count_) &&
		(session_.get_capture_state() != Session::Stopped)) {

		input_cond_.wait(input_lock);
	}

	// Return value is valid if we're not aborting the decode,
	return boost::make_optional(!interrupt_ &&
		// and there's more work to do...
		(samples_decoded_ < sample_count_ || !frame_complete_) &&
		// and if the end of the data hasn't been reached yet
		(!((samples_decoded_ >= sample_count_) && (session_.get_capture_state() == Session::Stopped))),
		sample_count_);
}

void DecoderStack::decode_data(
	const int64_t abs_start_samplenum, const int64_t sample_count, const unsigned int unit_size,
	srd_session *const session)
{
	const unsigned int chunk_sample_count =
		DecodeChunkLength / segment_->unit_size();

	for (int64_t i = abs_start_samplenum; !interrupt_ && i < sample_count;
			i += chunk_sample_count) {

		const int64_t chunk_end = min(
			i + chunk_sample_count, sample_count);
		const uint8_t* chunk = segment_->get_samples(i, chunk_end);

		if (srd_session_send(session, i, chunk_end, chunk,
				(chunk_end - i) * unit_size, unit_size) != SRD_OK) {
			error_message_ = tr("Decoder reported an error");
			delete[] chunk;
			break;
		}
		delete[] chunk;

		{
			lock_guard<mutex> lock(output_mutex_);
			samples_decoded_ = chunk_end;
		}
	}
}

void DecoderStack::decode_proc()
{
	optional<int64_t> sample_count;
	srd_session *session;
	srd_decoder_inst *prev_di = nullptr;

	assert(segment_);

	// Prevent any other decode threads from accessing libsigrokdecode
	lock_guard<mutex> srd_lock(global_srd_mutex_);

	// Create the session
	srd_session_new(&session);
	assert(session);

	// Create the decoders
	const unsigned int unit_size = segment_->unit_size();

	for (const shared_ptr<decode::Decoder> &dec : stack_) {
		srd_decoder_inst *const di = dec->create_decoder_inst(session);

		if (!di) {
			error_message_ = tr("Failed to create decoder instance");
			srd_session_destroy(session);
			return;
		}

		if (prev_di)
			srd_inst_stack (session, prev_di, di);

		prev_di = di;
	}

	// Get the intial sample count
	{
		unique_lock<mutex> input_lock(input_mutex_);
		sample_count = sample_count_ = segment_->get_sample_count();
	}

	// Start the session
	srd_session_metadata_set(session, SRD_CONF_SAMPLERATE,
		g_variant_new_uint64((uint64_t)samplerate_));

	srd_pd_output_callback_add(session, SRD_OUTPUT_ANN,
		DecoderStack::annotation_callback, this);

	srd_session_start(session);

	int64_t abs_start_samplenum = 0;
	do {
		decode_data(abs_start_samplenum, *sample_count, unit_size, session);
		abs_start_samplenum = *sample_count;
	} while (error_message_.isEmpty() && (sample_count = wait_for_data()));

	// Make sure all annotations are known to the frontend
	new_annotations();

	// Destroy the session
	srd_session_destroy(session);
}

void DecoderStack::annotation_callback(srd_proto_data *pdata, void *decoder_stack)
{
	assert(pdata);
	assert(decoder);

	DecoderStack *const ds = (DecoderStack*)decoder_stack;
	assert(ds);

	lock_guard<mutex> lock(ds->output_mutex_);

	const Annotation a(pdata);

	// Find the row
	assert(pdata->pdo);
	assert(pdata->pdo->di);
	const srd_decoder *const decc = pdata->pdo->di->decoder;
	assert(decc);

	auto row_iter = ds->rows_.end();

	// Try looking up the sub-row of this class
	const auto r = ds->class_rows_.find(make_pair(decc, a.format()));
	if (r != ds->class_rows_.end())
		row_iter = ds->rows_.find((*r).second);
	else {
		// Failing that, use the decoder as a key
		row_iter = ds->rows_.find(Row(decc));
	}

	assert(row_iter != ds->rows_.end());
	if (row_iter == ds->rows_.end()) {
		qDebug() << "Unexpected annotation: decoder = " << decc <<
			", format = " << a.format();
		assert(false);
		return;
	}

	// Add the annotation
	(*row_iter).second.push_annotation(a);

	// Notify the frontend every DecodeNotifyPeriod annotations
	if (ds->inc_annotation_count() % DecodeNotifyPeriod == 0)
		ds->new_annotations();
}

void DecoderStack::on_new_frame()
{
	begin_decode();
}

void DecoderStack::on_data_received()
{
	{
		unique_lock<mutex> lock(input_mutex_);
		if (segment_)
			sample_count_ = segment_->get_sample_count();
	}
	input_cond_.notify_one();
}

void DecoderStack::on_frame_ended()
{
	{
		unique_lock<mutex> lock(input_mutex_);
		if (segment_)
			frame_complete_ = true;
	}
	input_cond_.notify_one();
}

} // namespace data
} // namespace pv
