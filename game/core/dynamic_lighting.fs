#version 330

in vec3 fragPosition;
in vec2 fragTexCoord;
in vec4 fragColor;
in vec3 fragNormal;

uniform sampler2D texture0;
uniform vec4 colDiffuse;

uniform vec3 u_view_pos;

uniform vec3 u_sun_dir;
uniform vec3 u_sun_color;
uniform vec3 u_ambient_color;

uniform int u_use_flat_normals;

uniform int u_flashlight_active;
uniform vec3 u_flashlight_pos;
uniform vec3 u_flashlight_dir;
uniform vec3 u_flashlight_color;

uniform float u_muzzle_flash_intensity;
uniform vec3 u_muzzle_flash_pos;
uniform vec3 u_muzzle_flash_color;

out vec4 finalColor;

void main()
{
    vec4 texelColor = texture(texture0, fragTexCoord);
    vec4 baseColor = texelColor * colDiffuse * fragColor;

    if (baseColor.a < 0.1) discard;

    vec3 N;
    if (u_use_flat_normals != 0)
    {
        vec3 dX = dFdx(fragPosition);
        vec3 dY = dFdy(fragPosition);
        vec3 faceN = normalize(cross(dX, dY));
        if (dot(faceN, fragNormal) < 0.0) faceN = -faceN;
        N = faceN;
    }
    else
    {
        N = normalize(fragNormal);
    }

    vec3 V = normalize(u_view_pos - fragPosition);

    vec3 L_sun = normalize(u_sun_dir);
    float NdotL_sun = max(dot(N, L_sun), 0.0);

    float spec_sun = 0.0;
    if (u_use_flat_normals == 0)
    {
        vec3 H_sun = normalize(L_sun + V);
        spec_sun = pow(max(dot(N, H_sun), 0.0), 32.0) * NdotL_sun * 0.25;
    }

    vec3 totalLight = u_ambient_color + (u_sun_color * NdotL_sun) + (u_sun_color * spec_sun);

    if (u_flashlight_active != 0)
    {
        vec3 toLight = u_flashlight_pos - fragPosition;
        float dist = length(toLight);
        if (dist > 1e-4)
        {
            vec3 L_spot = toLight / dist;
            float cosTheta = dot(-L_spot, normalize(u_flashlight_dir));

            float innerCos = 0.970;
            float outerCos = 0.898;
            float spotIntensity = clamp((cosTheta - outerCos) / (innerCos - outerCos), 0.0, 1.0);

            if (spotIntensity > 0.0)
            {
                float attenuation = 1.0 / (1.0 + 0.075 * dist + 0.022 * dist * dist);
                float NdotL_spot = max(dot(N, L_spot), 0.0);

                vec3 H_spot = normalize(L_spot + V);
                float spec_spot = (u_use_flat_normals == 0) ? (pow(max(dot(N, H_spot), 0.0), 24.0) * NdotL_spot * 0.40) : 0.0;

                totalLight += u_flashlight_color * (NdotL_spot + spec_spot) * attenuation * spotIntensity;
            }
        }
    }

    if (u_muzzle_flash_intensity > 0.001)
    {
        vec3 toMuzzle = u_muzzle_flash_pos - fragPosition;
        float distM = length(toMuzzle);
        if (distM < 18.0)
        {
            vec3 L_muzzle = toMuzzle / max(distM, 1e-4);
            float NdotL_muzzle = max(dot(N, L_muzzle), 0.0);

            float attenMuzzle = 1.0 / (1.0 + 0.50 * distM + 0.16 * distM * distM);

            vec3 H_muzzle = normalize(L_muzzle + V);
            float spec_muzzle = (u_use_flat_normals == 0) ? (pow(max(dot(N, H_muzzle), 0.0), 48.0) * NdotL_muzzle * 0.85) : 0.0;

            vec3 muzzleLight = u_muzzle_flash_color * (NdotL_muzzle * 1.5 + spec_muzzle) * attenMuzzle * u_muzzle_flash_intensity;
            totalLight += muzzleLight;
        }
    }

    vec3 finalRgb = baseColor.rgb * totalLight;
    finalColor = vec4(finalRgb, baseColor.a);
}
