#include <iostream>
#include <ctime>
#include "Color.h"
#include "scenes/Scene.h"
#include "Renderer.h"
#include "Ray.h"
#include "tools/Random.h"
#include "IBL.h"

#include <sstream>

using namespace std;

namespace OmochiRenderer {

PathTracer::PathTracer(const Camera &camera, int samples, int supersamples)
  : m_camera(camera)
{
  init(camera, samples, samples, 1, supersamples, NULL, "");
}

PathTracer::PathTracer(const Camera &camera, int min_samples, int max_samples, int steps, int supersamples, RenderingFinishCallback *callback, const std::string &hdrFileForIBL)
  : m_camera(camera)
{
  init(camera, min_samples, max_samples, steps, supersamples, callback, hdrFileForIBL);
}

void PathTracer::init(const Camera &camera, int min_samples, int max_samples, int steps, int supersamples, RenderingFinishCallback *callback, const std::string &hdrFileForIBL)
{
  SetCamera(camera);
	m_min_samples = (min_samples);
  m_max_samples = max_samples;
  m_step_samples = steps;
	m_supersamples = (supersamples);
  m_previous_samples = 0;
  m_renderFinishCallback = callback;

  m_checkIntersectionCount = 0;
  if (hdrFileForIBL.empty()) {
    m_ibl.reset();
  } else {
    m_ibl.reset(new IBL(hdrFileForIBL));
  }
  m_result = new Color[m_camera.GetScreenHeight()*m_camera.GetScreenWidth()];
}

PathTracer::~PathTracer()
{
	delete [] m_result;
}

void PathTracer::RenderScene(const Scene &scene) {

  // スクリーン中心
  const Vector3 screen_center = m_camera.GetScreenCenterPosition();

  m_omittedRayCount = 0;
  m_absorbedInObjectRayCount = 0;
  m_previous_samples = 0;
  for (int samples=m_min_samples; samples<=m_max_samples; samples+=m_step_samples) {
    clock_t t1, t2;
    t1 = clock();
    m_checkIntersectionCount = 0;
    ScanPixelsAndCastRays(scene, m_previous_samples, samples);
    t2 = clock();
    m_previous_samples = samples;
    cerr << "samples = " << samples << " rendering finished." << endl;
    double pastsec = 1.0*(t2-t1)/CLOCKS_PER_SEC;
    cerr << "rendering time = " << (1.0/60)*pastsec << " min." << endl;
    cerr << "speed = " << m_checkIntersectionCount/pastsec*(samples-m_previous_samples) << " rays (intersection check)/sec" << endl;
    if (m_renderFinishCallback) {
      (*m_renderFinishCallback)(samples, m_result);
    }
  }
}

void PathTracer::ScanPixelsAndCastRays(const Scene &scene, int previous_samples, int next_samples) {
  m_processed_y_counts = 0;

  const size_t height = m_camera.GetScreenHeight();
  const size_t width = m_camera.GetScreenWidth();
  const Vector3 &screen_x(m_camera.GetScreenAxisXvector());
  const Vector3 &screen_y(m_camera.GetScreenAxisYvector());
  const Vector3 &screen_center(m_camera.GetScreenCenterPosition());

  // trace all pixels
  const double averaging_factor = next_samples * m_supersamples * m_supersamples;
#pragma omp parallel for num_threads(2)
  for (int y=0; y<height; y++) {
    Random rnd(y+1+previous_samples*height);
    for (int x=0; x<width; x++) {
      const int index = x + (height - y - 1)*width;

      Color accumulated_radiance;
       
      // super-sampling
      for (int sy=0; sy<m_supersamples; sy++) for (int sx=0; sx < m_supersamples; sx++) {
        // (x,y)ピクセル内での位置: [0,1]
        const double rx = (2.0*sx + 1.0)/(2*m_supersamples);
        const double ry = (2.0*sy + 1.0)/(2*m_supersamples);

        Vector3 target_position = screen_center +
          screen_x * ((x+rx)/width - 0.5) +
          screen_y * ((y+ry)/height - 0.5);
        Vector3 target_dir = target_position - m_camera.GetCameraPosition();
        target_dir.normalize();

        // (m_samples)回サンプリングする
        for (int s=previous_samples+1; s<=next_samples; s++) {
          accumulated_radiance += Radiance(scene, Ray(m_camera.GetCameraPosition(), target_dir), rnd, 0);
          m_omittedRayCount++;
        }
      }
      // img_n+c(x) = n/(n+c)*img_n(x) + 1/(n+c)*sum_{n+1}^{n+c}rad_i(x)/supersamples^2
      m_result[index] = m_result[index] * (static_cast<double>(previous_samples) / next_samples) + accumulated_radiance / averaging_factor;
    }
    m_processed_y_counts++;
    cerr << "y = " << y << ": " << static_cast<double>(m_processed_y_counts)/height*100 << "% finished" << endl;

  }
}

const static int MinDepth = 5;
const static int MaxDepth = 64;

Color PathTracer::Radiance(const Scene &scene, const Ray &ray, Random &rnd, const int depth) {
  Scene::IntersectionInformation intersect;

  m_checkIntersectionCount++;
  if (!scene.CheckIntersection(ray, intersect)) {
    //double v = fabs(ray.dir.dot(Vector3(0, 1, 0)));
    //double v = 0;
    if (m_ibl.get()) {
      //return m_ibl->SampleOriginal(ray.dir);
      return m_ibl->Sample(ray.dir);
    } else {
      return Vector3(0, 0, 0);
    }
  }

  Vector3 normal = intersect.hit.normal.dot(ray.dir) < 0.0 ? intersect.hit.normal : intersect.hit.normal * -1.0;
  Color income;

  // この if 文を有効にすると、直接光のみ考慮するようになる
  //if (depth >= 1) {
  //  return intersect.object->material.emission;
  //}

  double russian_roulette_probability = std::max(intersect.object->material.color.x, std::max(intersect.object->material.color.y, intersect.object->material.color.z)); // 適当
  if (depth > MaxDepth) {
    russian_roulette_probability *= pow(0.5, depth-MaxDepth);
  }
  if (depth > MinDepth) {
    if (rnd.nextDouble() >= russian_roulette_probability) {
      if (intersect.object->material.emission.lengthSq() == 0) {
        m_absorbedInObjectRayCount++;
      }
      return intersect.object->material.emission;
    }
  } else {
    russian_roulette_probability = 1.0; // no roulette
  }

  switch (intersect.object->material.reflection_type) {
    case Material::REFLECTION_TYPE_LAMBERT:
      income = Radiance_Lambert(scene, ray, rnd, depth, intersect, normal, russian_roulette_probability);
      break;
    case Material::REFLECTION_TYPE_SPECULAR:
      income = Radiance_Specular(scene, ray, rnd, depth, intersect, normal, russian_roulette_probability);
      break;
    case Material::REFLECTION_TYPE_REFRACTION:
      income = Radiance_Refraction(scene, ray, rnd, depth, intersect, normal, russian_roulette_probability);
      break;

  }
  
  
//  }

  return income;
}

Color PathTracer::Radiance_Lambert(const Scene &scene, const Ray &ray, Random &rnd, const int depth, Scene::IntersectionInformation &intersect, const Vector3 &normal, double russian_roulette_prob) {
  Vector3 w,u,v;

  w = normal;
  if (fabs(normal.x) > EPS) {
   u = Vector3(0,1,0).cross(w);
  } else {
   u = Vector3(1,0,0).cross(w);
  }
  v = w.cross(u);
  double u1 = rnd.nextDouble(); double u2 = rnd.nextDouble();
  // pdf is 1/PI
  //double r1 = PI*u1; // Φ
  //double r2 = 1-u2; // cosθ
  //double r3 = sqrt(1-r2*r2); // sinθ
  //double pdf = 1/PI;

  // pdf is cosθ/PI
  double r1 = 2*PI*u1;
  double r2 = sqrt(u2); // cosθ
  double r3 = sqrt(1-u2); // sinθ
  double pdf = r2/PI;

  //const double r1 = 2 * PI * rnd.nextDouble();
  //const double r2 = rnd.nextDouble(), r2s = sqrt(r2);
  //Vector3 dir = u * cos(r1) * r2s + v * sin(r1) * r2s + w * sqrt(1.0-r2);
  //dir.normalize();
  //weight = intersect.object->color / russian_roulette_probability;

  Vector3 dir = u*r3*cos(r1) + v*r3*sin(r1) + w*r2;
  dir.normalize();

  //Color weight = intersect.object->color / PI * r2 / pdf / russian_roulette_prob;
  Color weight = intersect.object->material.color / russian_roulette_prob;
  Color income = Radiance(scene, Ray(intersect.hit.position, dir), rnd, depth+1);
  return intersect.object->material.emission + Vector3(weight.x*income.x, weight.y*income.y, weight.z*income.z);
}

// 鏡面反射
Color PathTracer::Radiance_Specular(const Scene &scene, const Ray &ray, Random &rnd, const int depth, Scene::IntersectionInformation &intersect, const Vector3 &normal, double russian_roulette_prob) {
  Vector3 reflected_dir(ray.dir - normal*2*ray.dir.dot(normal));
  reflected_dir.normalize();
  Color income = Radiance(scene, Ray(intersect.hit.position, reflected_dir), rnd, depth+1);
  Color weight = intersect.object->material.color / russian_roulette_prob;
  return intersect.object->material.emission + Vector3(weight.x*income.x, weight.y*income.y, weight.z*income.z);

}

// 屈折面
Color PathTracer::Radiance_Refraction(const Scene &scene, const Ray &ray, Random &rnd, const int depth, Scene::IntersectionInformation &intersect, const Vector3 &normal, double russian_roulette_prob) {
  bool into = intersect.hit.normal.dot(normal) > 0.0;

  Vector3 reflect_dir = ray.dir - normal*2*ray.dir.dot(normal);
  reflect_dir.normalize();
  double n_vacuum = REFRACTIVE_INDEX_VACUUM;
  double n_obj = intersect.object->material.refraction_rate;
  double n_ratio = into ? n_vacuum/n_obj : n_obj/n_vacuum;

  double dot = ray.dir.dot(normal);
  double cos2t = 1-n_ratio*n_ratio*(1-dot*dot);

  if (cos2t < 0) {
    // 全反射
    Color income = Radiance(scene, Ray(intersect.hit.position, reflect_dir), rnd, depth+1);
    Color weight = intersect.object->material.color / russian_roulette_prob;
    return intersect.object->material.emission + Vector3(weight.x*income.x, weight.y*income.y, weight.z*income.z);
  }

  // 屈折方向
  Vector3 refract_dir( (ray.dir - normal*dot)*n_ratio - normal*sqrt(cos2t) );
  refract_dir.normalize();
  const Ray refract_ray(intersect.hit.position, refract_dir);

  // Fresnel の式
  double F0 = (n_obj-n_vacuum)*(n_obj-n_vacuum)/((n_obj+n_vacuum)*(n_obj+n_vacuum));
  double c = 1 - ( into ? -dot : -refract_dir.dot(normal) );  // 1-cosθ
  double Fr = F0 + (1-F0)*pow(1-c, 5.0);    // Fresnel (反射の割合)
  double n_ratio2 = n_ratio*n_ratio;  // 屈折前後での放射輝度の変化率
  double Tr = (1-Fr)*n_ratio2;        // 屈折直後→直前の割合

  Color income, weight;

  if (depth > 2) {
    // 反射 or 屈折のみ追跡
    const double reflect_prob = 0.1 + 0.8 * Fr;
    if (rnd.nextDouble() < reflect_prob) {
      // 反射
      income = Radiance(scene, Ray(intersect.hit.position, reflect_dir), rnd, depth+1) * Fr;
      weight = intersect.object->material.color / (russian_roulette_prob * reflect_prob);
    } else {
      // 屈折
      income = Radiance(scene, refract_ray, rnd, depth+1) * Tr;
      weight = intersect.object->material.color / (russian_roulette_prob * (1-reflect_prob));
    }
  } else {
    // 反射と屈折両方追跡
    m_omittedRayCount++;
    income =
        Radiance(scene, Ray(intersect.hit.position, reflect_dir), rnd, depth+1) * Fr +
        Radiance(scene, refract_ray, rnd, depth+1) * Tr;
    weight = intersect.object->material.color / russian_roulette_prob;
  }

  return intersect.object->material.emission + Vector3(weight.x*income.x, weight.y*income.y, weight.z*income.z);
}

std::string PathTracer::GetCurrentRenderingInfo() const {

  stringstream ss;
  ss << "(width, height) = (" << m_camera.GetScreenWidth() << ", " << m_camera.GetScreenHeight() << ")" << endl;
  ss << "previous samples / pixel = " << m_previous_samples << "x(" << m_supersamples << "x" << m_supersamples << ")" << endl;
  ss << "current rendering samples / pixel = " << (m_previous_samples+m_step_samples) << "x(" << m_supersamples << "x" << m_supersamples << ")" << endl;
  ss << "absorbed ray / omitted ray = " << m_absorbedInObjectRayCount << " / " << m_omittedRayCount;
  if (m_omittedRayCount != 0) {
    ss << " = " << static_cast<double>(m_absorbedInObjectRayCount*100.0) / m_omittedRayCount << "%";
  }
  ss << endl;
  ss << m_processed_y_counts*100.0/m_camera.GetScreenHeight() << "% finished." << endl;

  return ss.str();
}

}
