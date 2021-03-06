#pragma once

#include "intersections.h"

// CHECKITOUT
/**
 * Computes a cosine-weighted random direction in a hemisphere.
 * Used for diffuse lighting.
 */
__host__ __device__
glm::vec3 calculateRandomDirectionInHemisphere(glm::vec3 normal, thrust::default_random_engine &rng) {
	thrust::uniform_real_distribution<float> u01(0, 1);

	float up = sqrt(u01(rng)); // cos(theta)
	float over = sqrt(1 - up * up); // sin(theta)
	float around = u01(rng) * TWO_PI;

	// Find a direction that is not the normal based off of whether or not the
	// normal's components are all equal to sqrt(1/3) or whether or not at
	// least one component is less than sqrt(1/3). Learned this trick from
	// Peter Kutz.

	glm::vec3 directionNotNormal;
	if (abs(normal.x) < SQRT_OF_ONE_THIRD) {
		directionNotNormal = glm::vec3(1, 0, 0);
	}
	else if (abs(normal.y) < SQRT_OF_ONE_THIRD) {
		directionNotNormal = glm::vec3(0, 1, 0);
	}
	else {
		directionNotNormal = glm::vec3(0, 0, 1);
	}

	// Use not-normal direction to generate two perpendicular directions
	glm::vec3 perpendicularDirection1 =
		glm::normalize(glm::cross(normal, directionNotNormal));
	glm::vec3 perpendicularDirection2 =
		glm::normalize(glm::cross(normal, perpendicularDirection1));

	return up * normal
		+ cos(around) * over * perpendicularDirection1
		+ sin(around) * over * perpendicularDirection2;
}

__host__ __device__ float FresnelDielectric_Evaluate(float cosThetaI, float etaI, float etaT) 
{
	cosThetaI = glm::clamp(cosThetaI, -1.0f, 1.0f);

	bool entering = cosThetaI > 0.0f;
	float etaIb = etaI;
	float etaTb = etaT;
	if (!entering) {
		etaIb = etaT;
		etaTb = etaI;
		cosThetaI = glm::abs(cosThetaI);
	}

	float sinThetaI = glm::sqrt(glm::max(0.0f, 1 - cosThetaI * cosThetaI));
	float sinThetaT = etaIb / etaTb * sinThetaI;

	if (sinThetaT >= 1) {
		return 1.0f;
	}

	float cosThetaT = glm::sqrt(glm::max(0.0f, 1 - sinThetaT * sinThetaT));

	float Rparl = ((etaTb * cosThetaI) - (etaIb * cosThetaT)) /
		((etaTb * cosThetaI) + (etaIb * cosThetaT));
	float Rperp = ((etaIb * cosThetaI) - (etaTb * cosThetaT)) /
		((etaIb * cosThetaI) + (etaTb * cosThetaT));
	return (Rparl * Rparl + Rperp * Rperp) / 2;
}

__host__ __device__ void SpecularReflection_BxDF(PathSegment & pathSegment, glm::vec3 intersect, glm::vec3 normal, const Material &m) {
	pathSegment.color *= m.specular.color;
	pathSegment.ray.direction = glm::reflect(pathSegment.ray.direction, normal);
	pathSegment.ray.origin = intersect + (.001f) * pathSegment.ray.direction;
}


 __host__ __device__ void SpecularRefraction_BxDF(PathSegment & pathSegment, glm::vec3 intersect, glm::vec3 normal, const Material &m, thrust::default_random_engine &rng) {
	 thrust::uniform_real_distribution<float> u01(0, 1);

	// flip the normal and adjust the eta if we are leaving the  object
	glm::vec3 wo = pathSegment.ray.direction;
	bool leaving = glm::dot(wo, normal) > 0.f;
	glm::vec3 n = normal * (leaving ? -1.f : 1.f);
	float eta = leaving ? m.indexOfRefraction : (1.f / m.indexOfRefraction);
	glm::vec3 wi = glm::refract(wo, n, eta);

	// handle total internal reflection
	if (glm::length(wi) < .01f) {
		pathSegment.color *= 0.0f;
		wi = glm::reflect(wo, normal);
	}
	
	pathSegment.color *= m.specular.color;
	pathSegment.ray.direction = wi;
	pathSegment.ray.origin = intersect + (.001f) * pathSegment.ray.direction;
}

 __host__ __device__ void Glass_BxDF(PathSegment & pathSegment, glm::vec3 intersect, glm::vec3 normal, const Material &m, thrust::default_random_engine &rng) {
	 thrust::uniform_real_distribution<float> u01(0, 1);
	 float VdotN = glm::dot(-pathSegment.ray.direction, normal);
	 bool leaving = VdotN < 0.f;
	 float eI = leaving ? m.indexOfRefraction : 1.f;
	 float eT = leaving ? 1.f : m.indexOfRefraction;

	 float fresnel = FresnelDielectric_Evaluate(VdotN, eI, eT) / glm::abs(VdotN);

	 if (u01(rng) < fresnel) {
		 SpecularReflection_BxDF(pathSegment, intersect, normal, m);
	 }
	 else {
		 SpecularRefraction_BxDF(pathSegment, intersect, normal, m, rng);
	 }
 }

__forceinline__ __host__ __device__ void Lambert_BxDF(PathSegment & pathSegment,glm::vec3 intersect,glm::vec3 normal,const Material &m, thrust::default_random_engine &rng) {
	pathSegment.ray.direction = calculateRandomDirectionInHemisphere(glm::normalize(normal), rng);
	pathSegment.color *= m.color;
	pathSegment.ray.origin = intersect + (.001f) * pathSegment.ray.direction;
}
/**
 * Scatter a ray with some probabilities according to the material properties.
 * For example, a diffuse surface scatters in a cosine-weighted hemisphere.
 * A perfect specular surface scatters in the reflected ray direction.
 * In order to apply multiple effects to one surface, probabilistically choose
 * between them.
 * 
 * The visual effect you want is to straight-up add the diffuse and specular
 * components. You can do this in a few ways. This logic also applies to
 * combining other types of materias (such as refractive).
 * 
 * - Always take an even (50/50) split between a each effect (a diffuse bounce
 *   and a specular bounce), but divide the resulting color of either branch
 *   by its probability (0.5), to counteract the chance (0.5) of the branch
 *   being taken.
 *   - This way is inefficient, but serves as a good starting point - it
 *     converges slowly, especially for pure-diffuse or pure-specular.
 * - Pick the split based on the intensity of each material color, and divide
 *   branch result by that branch's probability (whatever probability you use).
 *
 * This method applies its changes to the Ray parameter `ray` in place.
 * It also modifies the color `color` of the ray in place.
 *
 * You may need to change the parameter list for your purposes!
 */
__host__ __device__
void scatterRay(
		PathSegment & pathSegment,
        glm::vec3 intersect,
        glm::vec3 normal,
        const Material &m,
        thrust::default_random_engine &rng) {
	if (m.hasReflective > EPSILON && m.hasRefractive > EPSILON) {
		Glass_BxDF(pathSegment, intersect, normal, m, rng);
	}
	else if (m.hasReflective > EPSILON) {
		SpecularReflection_BxDF(pathSegment, intersect, normal, m);
	}
	else if (m.hasRefractive > EPSILON) {
		SpecularRefraction_BxDF(pathSegment, intersect, normal, m, rng);
	}
	else {
		Lambert_BxDF(pathSegment, intersect, normal, m, rng);
	}
}
