/*
 * Copyright (c) 2014, Shogun Toolbox Foundation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:

 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from this
 * software without specific prior written permission.

 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * Written (W) 2014 Khaled Nasr
 */

#include <shogun/neuralnets/RBM.h>

#include <shogun/base/progress.h>
#include <shogun/mathematics/Math.h>
#include <shogun/mathematics/eigen3.h>
#include <shogun/mathematics/RandomNamespace.h>
#include <shogun/mathematics/NormalDistribution.h>

#include <utility>

using namespace shogun;

RBM::RBM() : RandomMixin<SGObject>()
{
	init();
}

RBM::RBM(int32_t num_hidden)
{
	init();
	m_num_hidden = num_hidden;
}

RBM::RBM(int32_t num_hidden, int32_t num_visible,
	ERBMVisibleUnitType visible_unit_type) : RandomMixin<SGObject>()
{
	init();
	m_num_hidden = num_hidden;
	add_visible_group(num_visible, visible_unit_type);
}

RBM::~RBM()
{
}

void RBM::add_visible_group(int32_t num_units, ERBMVisibleUnitType unit_type)
{
	m_num_visible_groups++;
	m_num_visible += num_units;

	m_visible_group_sizes.push_back(num_units);
	m_visible_group_types.push_back(unit_type);

	int32_t n = m_visible_state_offsets.size();

	if (n==0)
		m_visible_state_offsets.push_back(0);
	else
		m_visible_state_offsets.push_back(
		    m_visible_state_offsets[n - 1] + m_visible_group_sizes[n - 1]);
}

void RBM::initialize_neural_network(float64_t sigma)
{
	m_num_params = m_num_visible + m_num_hidden + m_num_visible*m_num_hidden;
	m_params = SGVector<float64_t>(m_num_params);
	random::fill_array(m_params, NormalDistribution<float64_t>(0.0, sigma), m_prng);
}

void RBM::set_batch_size(int32_t batch_size)
{
	if (m_batch_size == batch_size) return;

	m_batch_size = batch_size;

	hidden_state = SGMatrix<float64_t>(m_num_hidden,m_batch_size);
	visible_state = SGMatrix<float64_t>(m_num_visible,m_batch_size);

	reset_chain();
}

void RBM::train(std::shared_ptr<Features> features)
{
	require(features != NULL, "Invalid (NULL) feature pointer");

	auto dense_features = std::dynamic_pointer_cast<DenseFeatures<float64_t>>(features);

	require(dense_features, "Input features must be of type DenseFeatures<float64_t>.");
	require(dense_features->get_num_features()==m_num_visible,
		"Number of features ({}) must match the RBM's number of visible units "
		"({})", dense_features->get_num_features(), m_num_visible);

	SGMatrix<float64_t> inputs = dense_features->get_feature_matrix();

	int32_t training_set_size = inputs.num_cols;
	if (gd_mini_batch_size==0) gd_mini_batch_size = training_set_size;
	set_batch_size(gd_mini_batch_size);

	for (int32_t i=0; i<m_num_visible; i++)
		for (int32_t j=0; j<m_batch_size; j++)
			visible_state(i,j) = inputs(i,j);

	SGVector<float64_t> gradients(m_num_params);

	// needed for momentum
	SGVector<float64_t> param_updates(m_num_params);
	param_updates.zero();

	float64_t alpha = gd_learning_rate;

	SGMatrix<float64_t> buffer;
	if (monitoring_method == RBMMM_RECONSTRUCTION_ERROR)
		buffer = SGMatrix<float64_t>(m_num_visible, m_batch_size);
	else if (monitoring_method == RBMMM_PSEUDO_LIKELIHOOD)
		buffer = SGMatrix<float64_t>(m_num_hidden, m_batch_size);

	int32_t counter = 0;

	for (auto i : SG_PROGRESS(range(0, max_num_epochs)))
	{
		for (int32_t j=0; j < training_set_size; j += gd_mini_batch_size)
		{
			alpha = gd_learning_rate_decay*alpha;

			if (j+gd_mini_batch_size>training_set_size)
				j = training_set_size-gd_mini_batch_size;

			SGMatrix<float64_t> inputs_batch(inputs.matrix+j*inputs.num_rows,
				inputs.num_rows, gd_mini_batch_size, false);

			for (int32_t k=0; k<m_num_params; k++)
				m_params[k] += gd_momentum*param_updates[k];

			contrastive_divergence(inputs_batch, gradients);

			for (int32_t k=0; k<m_num_params; k++)
			{
				param_updates[k] = gd_momentum*param_updates[k]
						-alpha*gradients[k];

				m_params[k] -= alpha*gradients[k];
			}

			if (counter%monitoring_interval == 0)
			{
				if (monitoring_method==RBMMM_RECONSTRUCTION_ERROR)
					io::info("Epoch {}: reconstruction Error = {}",i,
						reconstruction_error(inputs_batch, buffer));
				if (monitoring_method==RBMMM_PSEUDO_LIKELIHOOD)
					io::info("Epoch {}: Pseudo-log-likelihood = {}",i,
						pseudo_likelihood(inputs_batch,buffer));
			}
			counter ++;
		}
	}
}

void RBM::sample(int32_t num_gibbs_steps,
	int32_t batch_size)
{
	set_batch_size(batch_size);

	for (int32_t i=0; i<num_gibbs_steps; i++)
	{
		mean_hidden(visible_state, hidden_state);
		sample_hidden(hidden_state, hidden_state);
		mean_visible(hidden_state, visible_state);
		if (i<num_gibbs_steps-1)
			sample_visible(visible_state, visible_state);
	}
}

std::shared_ptr<DenseFeatures< float64_t >> RBM::sample_group(int32_t V,
	int32_t num_gibbs_steps, int32_t batch_size)
{
	require(V<m_num_visible_groups,
		"Visible group index ({}) out of bounds ({})", V, m_num_visible);

	sample(num_gibbs_steps, batch_size);

	SGMatrix<float64_t> result(m_visible_group_sizes[V], m_batch_size);

	int32_t offset = m_visible_state_offsets[V];
	for (int32_t i = 0; i < m_visible_group_sizes[V]; i++)
		for (int32_t j=0; j<m_batch_size; j++)
			result(i,j) = visible_state(i+offset,j);

	return std::make_shared<DenseFeatures<float64_t>>(result);
}

void RBM::sample_with_evidence(
	int32_t E, std::shared_ptr<DenseFeatures< float64_t >> evidence, int32_t num_gibbs_steps)
{
	require(E<m_num_visible_groups,
		"Visible group index ({}) out of bounds ({})", E, m_num_visible);

	set_batch_size(evidence->get_num_vectors());

	SGMatrix<float64_t> evidence_matrix = evidence->get_feature_matrix();

	int32_t offset = m_visible_state_offsets[E];

	for (int32_t i = 0; i < m_visible_group_sizes[E]; i++)
		for (int32_t j=0; j<m_batch_size; j++)
			visible_state(i+offset,j) = evidence_matrix(i,j);

	for (int32_t n=0; n<num_gibbs_steps; n++)
	{
		mean_hidden(visible_state, hidden_state);
		sample_hidden(hidden_state, hidden_state);
		mean_visible(hidden_state, visible_state);
		if (n<num_gibbs_steps-1)
		{
			for (int32_t k=0; k<m_num_visible_groups; k++)
				if (k!=E)
					sample_visible(k, visible_state, visible_state);
		}

		for (int32_t i = 0; i < m_visible_group_sizes[E]; i++)
			for (int32_t j=0; j<m_batch_size; j++)
				visible_state(i+offset,j) = evidence_matrix(i,j);
	}
}

std::shared_ptr<DenseFeatures< float64_t >> RBM::sample_group_with_evidence(int32_t V,
	int32_t E, std::shared_ptr<DenseFeatures< float64_t >> evidence, int32_t num_gibbs_steps)
{
	require(V<m_num_visible_groups,
		"Visible group index ({}) out of bounds ({})", V, m_num_visible);
	require(E<m_num_visible_groups,
		"Visible group index ({}) out of bounds ({})", E, m_num_visible);

	sample_with_evidence(E, std::move(evidence), num_gibbs_steps);

	SGMatrix<float64_t> result(m_visible_group_sizes[V], m_batch_size);

	int32_t offset = m_visible_state_offsets[V];
	for (int32_t i = 0; i < m_visible_group_sizes[V]; i++)
		for (int32_t j=0; j<m_batch_size; j++)
			result(i,j) = visible_state(i+offset,j);

	return std::make_shared<DenseFeatures<float64_t>>(result);
}

void RBM::reset_chain()
{
	for (int32_t i=0; i<m_num_visible; i++)
		for (int32_t j=0; j<m_batch_size; j++)
			visible_state(i,j) = m_uniform_prob(m_prng) > 0.5;
}

float64_t RBM::free_energy(SGMatrix< float64_t > visible, SGMatrix< float64_t > buffer)
{
	set_batch_size(visible.num_cols);

	if (buffer.num_rows==0)
		buffer = SGMatrix<float64_t>(m_num_hidden, m_batch_size);

	typedef Eigen::Map<Eigen::MatrixXd> EMatrix;
	typedef Eigen::Map<Eigen::VectorXd> EVector;

	EMatrix V(visible.matrix, visible.num_rows, visible.num_cols);
	EMatrix W(get_weights().matrix, m_num_hidden, m_num_visible);
	EVector B(get_visible_bias().vector, m_num_visible);
	EVector C(get_hidden_bias().vector, m_num_hidden);

	EVector bv_buffer(buffer.matrix, m_batch_size);
	EMatrix wv_buffer(buffer.matrix, m_num_hidden, m_batch_size);

	bv_buffer = B.transpose()*V;
	float64_t bv_term = bv_buffer.sum();

	wv_buffer.colwise() = C;
	wv_buffer += W*V;

	float64_t wv_term = 0;
	for (int32_t i=0; i<m_num_hidden; i++)
		for (int32_t j=0; j<m_batch_size; j++)
			wv_term += std::log(1.0 + std::exp(wv_buffer(i, j)));

	float64_t F = -1.0*(bv_term+wv_term)/m_batch_size;

	for (int32_t k=0; k<m_num_visible_groups; k++)
	{
		if (m_visible_group_types[k] == RBMVUT_GAUSSIAN)
		{
			int32_t offset = m_visible_state_offsets[k];

			for (int32_t i = 0; i < m_visible_group_sizes[k]; i++)
				for (int32_t j=0; j<m_batch_size; j++)
					F += 0.5*Math::pow(visible(i+offset,j),2)/m_batch_size;
		}
	}

	return F;
}

void RBM::free_energy_gradients(SGMatrix< float64_t > visible,
	SGVector< float64_t > gradients,
	bool positive_phase,
	SGMatrix< float64_t > hidden_mean_given_visible)
{
	set_batch_size(visible.num_cols);

	if (hidden_mean_given_visible.num_rows==0)
	{
		hidden_mean_given_visible = SGMatrix<float64_t>(m_num_hidden,m_batch_size);
		mean_hidden(visible, hidden_mean_given_visible);
	}

	typedef Eigen::Map<Eigen::MatrixXd> EMatrix;
	typedef Eigen::Map<Eigen::VectorXd> EVector;

	EMatrix V(visible.matrix, visible.num_rows, visible.num_cols);
	EMatrix PH(hidden_mean_given_visible.matrix, m_num_hidden,m_batch_size);

	EMatrix WG(get_weights(gradients).matrix, m_num_hidden, m_num_visible);
	EVector BG(get_visible_bias(gradients).vector, m_num_visible);
	EVector CG(get_hidden_bias(gradients).vector, m_num_hidden);

	if (positive_phase)
	{
		WG = -1*PH*V.transpose()/m_batch_size;
		BG = -1*V.rowwise().sum()/m_batch_size;
		CG = -1*PH.rowwise().sum()/m_batch_size;
	}
	else
	{
		WG += PH*V.transpose()/m_batch_size;
		BG += V.rowwise().sum()/m_batch_size;
		CG += PH.rowwise().sum()/m_batch_size;
	}
}

void RBM::contrastive_divergence(SGMatrix< float64_t > visible_batch,
	SGVector< float64_t > gradients)
{
	set_batch_size(visible_batch.num_cols);

	// positive phase
	mean_hidden(visible_batch, hidden_state);
	free_energy_gradients(visible_batch, gradients, true, hidden_state);

	// sampling
	for (int32_t i=0; i<cd_num_steps; i++)
	{
		if (i>0 || cd_persistent)
			mean_hidden(visible_state, hidden_state);
		sample_hidden(hidden_state, hidden_state);
		mean_visible(hidden_state, visible_state);
		if (cd_sample_visible)
			sample_visible(visible_state, visible_state);
	}

	// negative phase
	mean_hidden(visible_state, hidden_state);
	free_energy_gradients(visible_state, gradients, false, hidden_state);

	// regularization
	if (l2_coefficient>0)
	{
		int32_t len = m_num_hidden*m_num_visible;
		for (int32_t i=0; i<len; i++)
			gradients[i+m_num_visible+m_num_hidden] +=
				l2_coefficient * m_params[i+m_num_visible+m_num_hidden];
	}

	if (l1_coefficient>0)
	{
		int32_t len = m_num_hidden*m_num_visible;
		for (int32_t i=0; i<len; i++)
			gradients[i+m_num_visible+m_num_hidden] +=
				l1_coefficient * m_params[i+m_num_visible+m_num_hidden];
	}

}

float64_t RBM::reconstruction_error(SGMatrix< float64_t > visible,
	SGMatrix< float64_t > buffer)
{
	set_batch_size(visible.num_cols);

	if (buffer.num_rows==0)
		buffer = SGMatrix<float64_t>(m_num_visible, m_batch_size);

	mean_hidden(visible, hidden_state);
	sample_hidden(hidden_state, hidden_state);
	mean_visible(hidden_state, buffer);

	float64_t error = 0;

	int32_t len = m_num_visible*m_batch_size;
	for (int32_t i=0; i<len; i++)
			error += Math::pow(buffer[i]-visible[i],2);

	return error/m_batch_size;
}


float64_t RBM::pseudo_likelihood(SGMatrix< float64_t > visible,
	SGMatrix< float64_t > buffer)
{
	for (int32_t k=0; k<m_num_visible_groups; k++)
		if (m_visible_group_types[k] != RBMVUT_BINARY)
			error("Pseudo-likelihood is only supported for binary visible units");

	set_batch_size(visible.num_cols);

	if (buffer.num_rows==0)
	buffer = SGMatrix<float64_t>(m_num_hidden, m_batch_size);

	SGVector<int32_t> indices(m_batch_size);
	random::fill_array(indices, 0, m_num_visible-1, m_prng);

	float64_t f1 = free_energy(visible, buffer);

	for (int32_t j=0; j<m_batch_size; j++)
		visible(indices[j],j) = 1.0-visible(indices[j],j);

	float64_t f2 = free_energy(visible, buffer);

	for (int32_t j=0; j<m_batch_size; j++)
		visible(indices[j],j) = 1.0-visible(indices[j],j);

	return m_num_visible * std::log(1.0 / (1 + std::exp(f1 - f2)));
}

void RBM::mean_hidden(SGMatrix< float64_t > visible, SGMatrix< float64_t > result)
{
	typedef Eigen::Map<Eigen::MatrixXd> EMatrix;
	typedef Eigen::Map<Eigen::VectorXd> EVector;

	EMatrix V(visible.matrix, visible.num_rows, visible.num_cols);
	EMatrix H(result.matrix, result.num_rows, result.num_cols);
	EMatrix W(get_weights().matrix, m_num_hidden, m_num_visible);
	EVector C(get_hidden_bias().vector, m_num_hidden);

	H.colwise() = C;
	H += W*V;

	int32_t len = result.num_rows*result.num_cols;
	for (int32_t i=0; i<len; i++)
		result[i] = 1.0 / (1.0 + std::exp(-1.0 * result[i]));
}

void RBM::mean_visible(SGMatrix< float64_t > hidden, SGMatrix< float64_t > result)
{
	typedef Eigen::Map<Eigen::MatrixXd> EMatrix;
	typedef Eigen::Map<Eigen::VectorXd> EVector;

	EMatrix H(hidden.matrix, hidden.num_rows, hidden.num_cols);
	EMatrix V(result.matrix, result.num_rows, result.num_cols);
	EMatrix W(get_weights().matrix, m_num_hidden, m_num_visible);
	EVector B(get_visible_bias().vector, m_num_visible);

	V.colwise() = B;
	V += W.transpose()*H;

	for (int32_t k=0; k<m_num_visible_groups; k++)
	{
		int32_t offset = m_visible_state_offsets[k];

		if (m_visible_group_types[k] == RBMVUT_BINARY)
		{
			for (int32_t i = 0; i < m_visible_group_sizes[k]; i++)
				for (int32_t j=0; j<m_batch_size; j++)
					result(i + offset, j) =
					    1.0 / (1.0 + std::exp(-1.0 * result(i + offset, j)));
		}
		if (m_visible_group_types[k] == RBMVUT_SOFTMAX)
		{
			// to avoid exponentiating large numbers, the maximum activation is
			// subtracted from all the activations and the computations are done
			// in thelog domain

			float64_t max = result(offset,0);
			for (int32_t i = 0; i < m_visible_group_sizes[k]; i++)
				for (int32_t j=0; j<m_batch_size; j++)
					if (result(i+offset,j) > max)
						max = result(i+offset,j);

			for (int32_t j=0; j<m_batch_size; j++)
			{
				float64_t sum = 0;
				for (int32_t i = 0; i < m_visible_group_sizes[k]; i++)
					sum += std::exp(result(i + offset, j) - max);

				float64_t normalizer = std::log(sum);

				for (int32_t i = 0; i < m_visible_group_sizes[k]; i++)
					result(i + offset, j) =
					    std::exp(result(i + offset, j) - max - normalizer);
			}
		}
	}
}

void RBM::sample_hidden(SGMatrix< float64_t > mean, SGMatrix< float64_t > result)
{
	int32_t length = result.num_rows*result.num_cols;
	for (int32_t i=0; i<length; i++)
		result[i] = m_uniform_prob(m_prng) < mean[i];
}

void RBM::sample_visible(SGMatrix< float64_t > mean, SGMatrix< float64_t > result)
{
	for (int32_t k=0; k<m_num_visible_groups; k++)
	{
		sample_visible(k, mean, result);
	}
}

void RBM::sample_visible(int32_t index,
	SGMatrix< float64_t > mean, SGMatrix< float64_t > result)
{
	int32_t offset = m_visible_state_offsets[index];

	if (m_visible_group_types[index] == RBMVUT_BINARY)
	{
		for (int32_t i = 0; i < m_visible_group_sizes[index]; i++)
			for (int32_t j=0; j<m_batch_size; j++)
				result(i+offset,j) = m_uniform_prob(m_prng) < mean(i+offset,j);
	}

	if (m_visible_group_types[index] == RBMVUT_SOFTMAX)
	{
		for (int32_t i = 0; i < m_visible_group_sizes[index]; i++)
			for (int32_t j=0; j<m_batch_size; j++)
				result(i+offset,j) = 0;

		UniformIntDistribution<int32_t> uniform_int_dist(0, 1);
		for (int32_t j=0; j<m_batch_size; j++)
		{
			int32_t r = uniform_int_dist(m_prng);
			float64_t sum = 0;
			for (int32_t i = 0; i < m_visible_group_sizes[index]; i++)
			{
				sum += mean(i+offset,j);
				if (r<=sum)
				{
					result(i+offset,j) = 1;
					break;
				}
			}
		}
	}
}


SGMatrix< float64_t > RBM::get_weights(SGVector< float64_t > p)
{
	if (p.vlen==0)
		return SGMatrix<float64_t>(m_params.vector+m_num_visible,
			m_num_hidden, m_num_visible, false);
	else
		return SGMatrix<float64_t>(p.vector+m_num_visible,
			m_num_hidden, m_num_visible, false);
}

SGVector< float64_t > RBM::get_hidden_bias(SGVector< float64_t > p)
{
	if (p.vlen==0)
		return SGVector<float64_t>(m_params.vector+m_num_visible+m_num_visible*m_num_hidden,
			m_num_hidden, false);
	else
		return SGVector<float64_t>(p.vector+m_num_visible+m_num_visible*m_num_hidden,
			m_num_hidden, false);
}

SGVector< float64_t > RBM::get_visible_bias(SGVector< float64_t > p)
{
	if (p.vlen==0)
		return SGVector<float64_t>(m_params.vector, m_num_visible, false);
	else
		return SGVector<float64_t>(p.vector, m_num_visible, false);
}

void RBM::init()
{
	cd_num_steps = 1;
	cd_persistent = true;
	cd_sample_visible = false;
	l2_coefficient = 0.0;
	l1_coefficient = 0.0;
	monitoring_method = RBMMM_RECONSTRUCTION_ERROR;
	monitoring_interval = 10;

	gd_mini_batch_size = 0;
	max_num_epochs = 1;
	gd_learning_rate = 0.1;
	gd_learning_rate_decay = 1.0;
	gd_momentum = 0.9;

	m_num_hidden = 0;
	m_num_visible = 0;
	m_num_visible_groups = 0;
	m_visible_group_sizes.clear();

	m_visible_group_types.clear();

	m_visible_state_offsets.clear();

	m_num_params = 0;
	m_batch_size = 0;

	m_uniform_prob.param({0.0, 1.0});

	SG_ADD(&cd_num_steps, "cd_num_steps", "Number of CD Steps");
	SG_ADD(&cd_persistent, "cd_persistent", "Whether to use PCD");
	SG_ADD(
	    &cd_sample_visible, "sample_visible",
	    "Whether to sample the visible units during (P)CD");
	SG_ADD(&l2_coefficient, "l2_coefficient", "L2 regularization coeff");
	SG_ADD(&l1_coefficient, "l1_coefficient", "L1 regularization coeff");
	SG_ADD(&monitoring_interval, "monitoring_interval", "Monitoring Interval");

	SG_ADD(
	    &gd_mini_batch_size, "gd_mini_batch_size",
	    "Gradient Descent Mini-batch size");
	SG_ADD(&max_num_epochs, "max_num_epochs", "Max number of Epochs");
	SG_ADD(
	    &gd_learning_rate, "gd_learning_rate",
	    "Gradient descent learning rate");
	SG_ADD(
	    &gd_learning_rate_decay, "gd_learning_rate_decay",
	    "Gradient descent learning rate decay");
	SG_ADD(&gd_momentum, "gd_momentum", "Gradient Descent Momentum");

	SG_ADD(&m_num_hidden, "num_hidden", "Number of Hidden Units");
	SG_ADD(&m_num_visible, "num_visible", "Number of Visible Units");

	SG_ADD(
	    &m_num_visible_groups, "num_visible_groups",
	    "Number of Visible Unit Groups");
	SG_ADD(
	    &m_visible_group_sizes, "visible_group_sizes",
	    "Sizes of Visible Unit Groups");
	SG_ADD(
	    &m_visible_group_types, "visible_group_types",
	    "Types of Visible Unit Groups");
	SG_ADD(
	    &m_visible_state_offsets, "visible_group_index_offsets",
	    "State Index offsets of Visible Unit Groups");

	SG_ADD(&m_num_params, "num_params", "Number of Parameters");
	SG_ADD(&m_params, "params", "Parameters");

	SG_ADD_OPTIONS(
	    (machine_int_t*)&monitoring_method, "monitoring_method",
	    "Monitoring Method", ParameterProperties::NONE,
	    SG_OPTIONS(RBMMM_RECONSTRUCTION_ERROR, RBMMM_PSEUDO_LIKELIHOOD));
}
